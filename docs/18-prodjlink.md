# 18 — Pro DJ Link

`rbp` carries Pioneer's whole Pro DJ Link stack, and on this unit it has never
run, because its network introspection is hardcoded to an interface that is down.
`netshim.so` points that introspection at the interface that is actually up.

**Status: built, deployed, and run on the unit. The substitution works, is
verified on the wire, and changes nothing downstream** — because rbp rewrites
its one `eth0` request onto `wlan0` and then never opens UDP 50000. The Link stack
is constructed and never commanded up.

**The blocker was then found, and it is rbp's own door, not a shim's** — see
[The gate, and the call behind it](#the-gate-and-the-call-behind-it).
`NetworkManager::operateConnectNetwork` `@0x38f830` **does** have a caller: it is
virtual, and `NetworkMonitor::timerCallback` reaches it once a second. What holds
Link down is the byte in front of that call — `ui::PcController::isUsbBConnected()`
(`PcController+0x72`), set only by `EnUsbMountMessage 3`. rbp raises exactly that
message **from its own FIFO protocol**, so the trigger is one word written to
`/tmp/udev_usb1`, using the channel `usb-watch.sh` already writes to.

An earlier version of this page said `operateConnectNetwork` had no caller at
all. That was read off a `bl` scan, and it is wrong — see that section for how the
scan fooled itself.

The substitution itself is still disarmed (`RB_NETALIAS=0` in `rb.local.conf`) and
the FIFO trigger has **not been fired**: joining the live Pro DJ Link network
while the CDJ-3000 at 192.168.1.201 is up is an outward-facing act and is the
operator's call to make, not a shim's. Everything below is the chain as decoded
off the binary and read back against the live process — vptr `0x4e107c`, `NM+0x24`
zero, `PcController+0x72` zero, nothing bound to UDP 50000.

Stages 0, 1, 2 and 2b in [Verification](#verification) have all been run. Stage 2
is a negative result with a positive cause, and 2b retires the one risk that
would have made every other question moot.

This supersedes the earlier verdict that Pro DJ Link was out of reach — see
[Not in scope](#not-in-scope) for exactly what that verdict did and did not say.

## What rbp carries

*Measured* (strings and the symbol table of the extracted v1.20 `rbp`):

* A full `network::ProDjLink` class set — `LinkDeviceManager`,
  `SystemManagement*`, the device-announcement and beat-packet code — plus
  `ui::MacAddressListener`, `ui::MacAddressHolder`, `ui::PcControlMacAddress`,
  `NetworkManager` and `NetworkMonitor`.
* The SOURCE screen names `LINK`, `rekordbox`, `rekordbox mobile`,
  `USB at PLAYER%d` and `SD at PLAYER%d` as sources, and all five slots carry a
  LINK image (`Obj_CTRL_SOURCE_GRP_SOURCE1_IMG_LINK_27` … `SOURCE5_IMG_LINK_87`).
* **No Beatport, and no streaming client of any kind** — zero strings. Asking
  for Beatport as a source is asking for code that is not in the binary.

And there is something to link *with*. A passive 20 s listen on UDP 50000 **run
from the unit** (*measured 2026-10-07*, `/tmp/plcap.py`) saw the LAN's own Pro DJ
Link traffic — 54-byte packets, magic `Qspt1WmJOL`, type `0x06`:

| source | names announced | rate |
|---|---|---|
| 192.168.1.201 | `CDJ-3000` **and** `NXS-GW`, alternating | one of each per ~1.5 s |
| 192.168.1.59 | `rekordbox` | ~1 per 2 s |

The CDJ is **one box announcing two identities** — a player identity and a
gateway identity — differing only in the name, `0x21`, `0x24`, `0x25`, `0x30`,
`0x34` and `0x35`. An earlier capture from the workstation attributed both names
to 192.168.1.100; today both come from 192.168.1.201, so that attribution was
wrong and the table above is the measurement to trust. (The `rekordbox` device's
announced MAC, `d0:11:e5:1c:cb:05`, is also **not** its ARP MAC,
`32:c3:5c:0a:44:bf` — a locally-administered address, i.e. a host with MAC
randomisation. Do not expect a peer's announced MAC to match its interface.)

So the work is end-to-end verifiable rather than hopeful.

## Why it does not work today

The literal string `eth0` sits at VA `0x3FE678` and is referenced from **exactly
twelve** functions. *Measured 2026-10-07* by disassembling
`extracted/XDJRX3/pdj/rbp` (md5 `4f2efcfc0c9e3f539289f863acfddcc6`, the stock
v1.20 binary) and resolving each `.word 0x003fe678` site to its enclosing
symbol — twelve sites, one function each:

| function | address |
|---|---|
| `get_ipaddr` | `0x39e77c` |
| `get_ip_inf` | `0x39e9fc` |
| `get_subnet_inf` | `0x39ea9c` |
| `setIpAddr` | `0x39dfb4` |
| `getMacAddr` | `0x39e1c4` |
| `get_myip_info` | `0x188b74` |
| `ui::PcControlMacAddress::getIpAddress` | `0x2e9c50` |
| `NetworkManager::getMacAddress` | `0x39129c` |
| `NetworkManager::getMacAddressUint8` | `0x391588` |
| `NetworkManager::getCurrentIpAddress` | `0x3919c4` |
| `NetworkMonitor::checkNetworkConnectionChange` | `0x392084` |
| `ui::MacAddressListener::run` | `0x340de0` |

On this unit `eth0` is **DOWN with no carrier**, while `wlan0` is **UP at
192.168.1.239/24**. Every one of those twelve functions therefore asks the kernel
about an interface with no address, and rbp concludes it has no network.

> **This is not the reason Link is down on this unit, and it was read as if it
> were.** It is a real defect and the shim is still the right answer to it, but it
> is *downstream* of a stop that happens first: `timerCallback` reads the gate
> through `IUiObjManager::getPcController()`, which the `rbp-nopc` stage stubs to
> `mov r0,#0`, so the connect branch is never taken and not one of these twelve
> functions is ever called. Measured 2026-10-07 — see
> [Run 2026-10-07](#run-2026-10-07-the-gate-rises-and-nothing-follows). Fix the
> stub first, or the shim is being asked to repair a road nothing drives on.

They reach the kernel through only **three** libc calls, all of them undefined
`GLIBC_2.4` imports of `rbp`, and therefore all three interposable:

| call | used by | for |
|---|---|---|
| `ioctl` | eleven of the twelve | `SIOCGIFADDR`, `SIOCGIFNETMASK`, `SIOCGIFHWADDR`, `SIOCGIFFLAGS`, `SIOCGIFINDEX` |
| `if_nametoindex` | `ui::MacAddressListener::run` | the index of a raw `AF_PACKET`/`ETH_P_ALL` socket |
| `system` | `network::Autoip`, `ui::MacAddressHolder` | `udhcpc -i eth0 -T 2 -t 3 -n -q` and `ifconfig \| awk '$1 ~ /^eth/ {print $NF}'` |

**There is no `SO_BINDTODEVICE` anywhere in `rbp`.** No socket is pinned to an
interface, so UDP egress already follows the routing table and already leaves via
`wlan0`. Only the eth0-*named* introspection is broken, which is why this is a
shim and not a patch.

## What rbp asks for, and what it does with the answer

Both unit stages ran on 2026-10-07, and they are why this document ends where it
does.

*Measured* (Stage 1, shadow — `RB_NETALIAS=0`, `RB_NETALIAS_LOG=1`, read back
from `/tmp/netshim.log`): rbp makes **exactly one** `ifreq` request in its whole
life — `SIOCGIFHWADDR` (`0x8927`) on `eth0`, once. There is no `SIOCGIFADDR`, no
`SIOCGIFINDEX`, no `SIOCGIFNETMASK`, no `if_nametoindex` line and no `system`
line. **Eleven of the twelve functions in the table above are never reached.**
The fourth predicted call does not exist: the real answer is *narrower* than the
table, not wider.

*Measured* (Stage 2, active — `RB_NETALIAS=1`): the rewrite fires exactly as
designed, and then nothing happens at all.

```
NETALIAS v1 pid=… armed … on=1 … alias=wlan0 src=auto
NETALIAS v1 pid=… ioctl req=0x8927 name="eth0" -> rewritten
```

The log freezes there. No `UDP 50000`, no `AF_PACKET` socket, no further request
of any kind. **rbp never asks the kernel for an address**, so an address is not
what it was waiting for.

**Why, from the binary.** Three facts, and they agree:

* `network::NetworkIf::postMessage(EnNetworkCommandType, int, int, int)`
  @ `0x38f04c` — the helper that would put a command into the `NetworkManager`
  pump — has **zero callers**.
* `NetworkIf::getInstance` @ `0x38f7cc` has exactly **two** callers, `0x2bcdd8`
  and `0x2bd034`, and both sit on the `getMacAddress` path. That path is the one
  `SIOCGIFHWADDR` measured above: the measured call and the referenced helper are
  the same piece of code, which is why the shim's rewrite reaches exactly one
  request and stops.
* `NetworkManager::operateConnectNetwork(bool)` @ `0x38f830` has no `bl` caller
  at all. It is a virtual method reached through a callback interface (the
  `_ZThn8_` thunks at `0x38f828`), not a handler in the message pump.

The stack is genuinely *constructed*: `NetworkManager`'s constructor builds
`ProDjLink`, `NetworkMonitor` and a `juce::Thread` unconditionally, and the thread
appears in the process's thread list. It is equally genuinely *idle*: its
`run()` @ `0x391230` is a blocking `recvCommonMessage(-1)` → `operate(msg)` pump,
and `operate()` @ `0x391094` dispatches a ten-entry jump table that nothing ever
feeds. (One caveat, stated because it is the only way this could be wrong: the
queue is a `common::CommonMessageQueue`, so in principle another subsystem could
post into it without going through `postMessage`. "Zero callers" rules out that
C++ helper, not the message type itself.)

**Nothing in rbp asks the network to come up.** That is the blocker, it is
upstream of every call this shim interposes, and a shim cannot reach it.

## What the shim does

`netshim.so` interposes those three calls, rewrites the interface **name** in
requests that name `eth0`, and delegates everything else. It never touches a
socket, a route, or a packet.

### Choosing the interface — four rules, in order

1. `NETALIAS_IFACE` set and names a present interface → **use it.** An explicit
   choice always wins.
2. `eth0` present and `IFF_UP|IFF_RUNNING` → **identity, do nothing.** If
   ethernet is ever plugged in, rbp works natively and the shim disables itself.
3. Otherwise the first candidate that is `UP|RUNNING`, not loopback, not `eth0`,
   and has an IPv4 address — preferring a `wl*` name.
4. Otherwise **identity** — every hook passes through.

It never fabricates a name. Rewriting `eth0` to an interface that does not exist
makes rbp worse off than leaving it alone, so identity is the only fallback that
is always correct. Selection happens once, in the constructor, and is frozen.

One ordering detail that is deliberate: `getifaddrs()` is itself a caller of
`ioctl(SIOCGIFCONF)`, so selection runs **before** the interposers are armed. If
it did not, the shim's own probe would appear in the inventory log as if rbp had
made it.

### The `ioctl` rewrite is a WHITELIST

Rewrite only when the request is whitelisted **and** `ifr_name` is exactly
`eth0` **and** an alias is active. Nothing else is inspected, and on every other
path the argument is never dereferenced.

| request | value | ship? | why |
|---|---|---|---|
| `SIOCGIFINDEX` | `0x8933` | **yes** | measured (`MacAddressListener::run`) |
| `SIOCGIFADDR` | `0x8915` | **yes** | measured (`get_ipaddr`, `get_myip_info`) |
| `SIOCGIFNETMASK` | `0x891b` | **yes** | measured (`get_subnet_inf`) |
| `SIOCGIFHWADDR` | `0x8927` | **yes** | measured (`getMacAddr`) |
| `SIOCGIFFLAGS` | `0x8913` | yes | a "is eth0 up?" test would otherwise veto everything |
| `SIOCGIFBRDADDR` | `0x8919` | yes | companion to the netmask |
| `SIOCGIFDSTADDR` `SIOCGIFMETRIC` `SIOCGIFMTU` `SIOCGIFTXQLEN` | `0x8917` `0x891d` `0x8921` `0x8942` | yes | harmless companions |
| `SIOCGIFCONF` | `0x8912` | **no** | `ifr_name` is an *output* buffer |
| `SIOCGIFNAME` | `0x8910` | **no** | index→name, the opposite direction |
| every `SIOCSIF*` setter | `0x8916`… | **never** | see below |
| everything else (GPIO, InterCpuCom, touch panel, the framebuffer range) | — | **no** | not an `ifreq`; must not be read |

The name is swapped in place around the call and **restored after it**, so the
kernel's answer lands in the caller's struct while the caller's own `ifr_name` is
left exactly as it set it. The request code itself is passed through unmodified —
these are plain `0x89xx` defines, not `_IOR`-encoded.

> **Why a whitelist and not a blacklist.** One of the twelve functions is
> **`setIpAddr`**, whose name and use of the `eth0` literal imply
> `ioctl(SIOCSIFADDR)`. Retargeting *that* at `wlan0` would write rbp's idea of an
> address — plausibly a 169.254.x.x link-local when `Autoip` runs — onto the live
> WiFi interface carrying the operator's SSH session and the unit's LAN access.
> With the whitelist, `SIOCSIFADDR("eth0")` reaches the kernel unchanged and
> fails harmlessly on the carrierless interface. **`wlan0` is never written to.**
> This is the single failure in this design that is not merely "a feature that
> does not work", and it is why the getter/setter split is enumerated rather than
> expressed as a prefix test: `SIOCGIF*` is not a prefix, it is a family that
> contains both.

### `if_nametoindex`

Substitute the name, delegate, return 0 if the real pointer did not resolve. The
index is **never cached** — it is stable only while the interface exists, and the
real call returning 0 for a vanished interface is already what the caller handles.

### `system` — an observing pass-through

It ships logging each distinct command line once, then calling the real
`system`. The refuse/rewrite rules exist and are unit-tested but are **gated
behind `NETALIAS_MAC_SCRAPE`, default 0**:

* `udhcpc …` → **never rewritten.** Running a DHCP client on `wlan0` would
  solicit a new lease on the interface carrying the operator's live WiFi. The
  rule is deliberately broader than the measured command — *any* command line
  containing `udhcpc` is refused — so there is no pattern to get wrong. Refusal
  returns `127 << 8` (the shell's "did not run") and leaves `errno` untouched.
* `ifconfig | awk '$1 ~ /^eth/ …'` → read-only, and its output is redirected to a
  temp file JUCE reads afterwards, so the only correct non-pass verdict is an
  **in-place** rewrite of `/^eth/` to `/^<iface>/` with the redirect intact.

Neither command needs a rewrite for Link to work, and neither may even exist in
the vendor rootfs — that is a measurement for the unit stage, not an assumption.

Worth knowing when reading the Stage-1 log: rbp's `system()` children inherit
`LD_PRELOAD`, so the shell and the command it runs are **also** loaded with the
shim. That is true of every shim in the list and not new here, but it means the
`system` interposer in the parent is the right place to decide, and the child's
own copy is inert besides.

## The load-order contract with `fbshim`

**`fbshim.so` already defines `ioctl`** ([`fb_shim.c`](../scripts/shims/fb_shim.c)),
and a process resolves `ioctl` to the *first* preloaded object that defines it.
Both directions of failure are bad:

* `netshim` **after** `fbshim` → fbshim's `ioctl` wins and the network rewrite
  silently does nothing, while `if_nametoindex` and `system` still work. A silent
  partial failure.
* `netshim` **before** `fbshim` **without delegating** → the framebuffer
  emulation, the tsc2007 ioctls and the `/dev/mem`/`gpiodrv` stubs never run.
  The display breaks.

So `netshim.so` goes **second**, after `crashcatch.so` and before `fbshim.so`,
and resolves `dlsym(RTLD_NEXT, "ioctl")` in its constructor. Everything it does
not rewrite is delegated to fbshim's `ioctl`, which passes `SIOCGIF*` straight to
the kernel anyway (its non-fb path is `(request & 0xffff) < 0x4600 || > 0x4620`,
and `SIOCGIF*` is `0x89xx`). fbshim does **not** chain via `dlsym` — it calls
`syscall(SYS_ioctl, …)` — so the chain terminates correctly at the kernel:
**netshim → fbshim → syscall**.

`doctor.sh` checks the *position* (not a substring) and prints a paste-ready fix
if the two are the wrong way round.

## Enablement — opt-in

`netshim.so` is installed and listed in `RB_LD_PRELOAD` from the start, so the
load-order contract is exercised on every launch and a wrong order is caught by
`doctor.sh` before it matters. The substitution itself defaults **off**.

| `rb.conf` | default | meaning |
|---|---|---|
| `RB_NETALIAS` | `0` | the substitution. `0` makes every hook an identity pass-through |
| `RB_NETALIAS_IFACE` | *(empty)* | force an interface, overriding the four rules |
| `RB_NETALIAS_MAC_SCRAPE` | `0` | arm the `system` verdicts (refuse `udhcpc`, rewrite the `ifconfig` scrape) |
| `RB_NETALIAS_LOG` | `0` | write `/tmp/netshim.log` |
| `RB_NETALIAS_CONNECT` | `0` | arm the **shim-side** one-shot bring-up (poll for a request file, call into rbp). Superseded by the FIFO route; leave `0`. Forces the log on |
| `RB_NETALIAS_CONNECT_FILE` | `/tmp/rb_link.req` | the file `RB_NETALIAS_CONNECT` watches for |

Turn it on in **`rb.local.conf`**, the file the tree already designates for
measured on-unit values, because `rb.conf` is overwritten on every install.
Empty means unset, as for every shim variable (`envutil.h`'s `env_flag`/
`env_str`, never a `getenv() != NULL` presence test) — and with the flag off, a
failed experiment is *inert* rather than merely disabled.

**`rb.local.conf` on the unit now reads `RB_NETALIAS=1`** (and
`RB_NETALIAS_CONNECT=1` from before the FIFO route was found). That is a unit-local
edit, not a commit: the tree's `rb.conf` still ships `RB_NETALIAS=0`. The live value
is what Stages 1–2b were run against, and it is why the shim log on the unit shows
a live alias rather than an identity pass-through.

## The gate, and the call behind it

*Measured* against `extracted/stock-rbp/rbp` (md5 `4f2efcfc…`, the stock v1.20
binary), which is non-PIE, so every address below is absolute and the same in
every process. The vptr and the four fields below were then read back out of the
running process at pid 28302, and agree.

Stage 2 left one open question: rbp believes it has an address and still does not
open UDP 50000, so *something inside rbp* must be missing. It is one byte — a
byte that a `bl` scan cannot see, which is why an earlier reading of this page got
the answer backwards.

```
0x38f830  NetworkManager::operateConnectNetwork(bool)
  38f830    ldrb  r3, [r0, #196]      ; [+0xc4], the bool main passed to initialize()
  38f838    cmp   r3, #0
  38f844    bne   38f8fc             ; -> Autoip::postDhcpMessage() on [+0xc0], then FALLS THROUGH
  38f848    ldr   r3, [r4, #36]      ; [+0x24] = NetworkMonitor's IPv4 address
  38f854    beq   38f8f4             ; RETURN -- a SILENT no-op while it is zero
  ...                               ; decode address [r4+36] / netmask [r4+40]
  38f89c    bl    ProDjLink::setIpAddress()
  38f8a8    bl    ProDjLink::setNetMask()
  38f8d0    str   #2, [r6, #24]      ; msg->+24 = 2
  38f8f0    bl    ProDjLink::operate(msg)
```

From there: `ProDjLink::operate` `@0x392b14` → `SystemManager::operateMessage`
`@0x3936a4`, which dispatches on `+24` **directly** and takes **2 = CONNECT**
(`0x39371c`) → `UdpServer::start()` `@0x393c88` → `UdpServer::run()` `@0x3938d4`,
which is where `setsockopt` + `bind` to **UDP 50000** happen (`0x39394c`/
`0x39395c`). Type 3 is DISCONNECT and is never built.

**Do not confuse the two dispatchers.** `NetworkManager::operate` `@0x391094`
dispatches on `+24 - 1` over a ten-entry table in which **2 is browseControlEnd**;
`SystemManager::operateMessage` dispatches on `+24` with **2 = CONNECT**. The same
number means opposite things five functions apart.

**This section used to say the opposite, and the way it was wrong is worth
keeping.** It said `operateConnectNetwork` "has no caller anywhere in the binary",
on the strength of a scan for a `bl` whose target is `0x38f830`. No such `bl`
exists — and none should: the function is **virtual**, so the only way to reach it
is `blx` through a vtable, and a vtable call leaves no `bl` to find. The scan
answered a narrower question than the one being asked, and the difference was read
as "nothing calls it". The function has a caller, and the caller runs **once a
second**; what has been missing is only the byte in front of it.

`NetworkMonitor` is a base subobject at `NetworkManager+8`. Its live vptr is the
secondary vtable whose address point is `0x4e107c`, read off the running process
(`[NM+8] == 0x4e107c`, with `[NM] == 0x4e0fa0`). Its slots are `+0`/`+4` the two
destructors, `+8` `timerCallback`, `+12` `operateConnectNetwork`, `+16`
`operateChangeNetwork`, `+20` `operateDisconnectNetwork`, `+24`
`notifyIpSettings`. `NetworkMonitor::timerCallback` `@0x392160` dispatches through
exactly those slots: `blx r3` on `[vptr+12]` at `0x3921f0`, and again at `0x39229c`.

`startNetwork` `@0x391c98`, `stopNetwork` and `startNetworkThread` `@0x390f4c`
remain uncalled, and `NetworkIf::postMessage` `@0x38f04c` still has zero callers —
so the older `NetworkIf` route really is dead. It is simply not the route that
matters. `NetworkManager::run()` `@0x391230` is still reached only as a
`juce::Thread` vtable entry.

**And the timer that drives it is already running.** `main` `@0x1045c` calls
`NetworkManager::initialize(bool)` `@0x391860`, which at `0x39192c` calls
`NetworkMonitor::startMonitoring` `@0x391f58` **with no branch around it** — the
`-a` bool only decides whether the `Autoip` object at `[+0xc0]` is constructed, and
`Autoip::start()` is called at `0x391914` before the monitor either way.
`startMonitoring` zeroes `[+0x1c]` (the address) and `[+0x24]` (the flag), then ends
in `juce::Timer::startTimer(this, 1000)`. `MSEC_TIMER_INTERVAL` `@0x4e126c` is
`1000`, and `MSEC_ERROR_MONITOR_INTERVAL` `@0x4e1268` is `90000`.

### The gate is `PcController+0x72`

`NetworkMonitor::timerCallback` `@0x392160`, decoded whole:

```
r1 = this[+0x24]                          ; byte, cached "connected"
r3 = getPcController() ? pcc[+0x72] : 0
if (r1 != 0) {                           ; ---- connected branch -------------
    if (r3 == 0) { this[+0x24]=0; vptr[+20]() /* operateDisconnectNetwork */ ; return }
    if (this[+0x48] && this[+0x4c] > 11999)  vptr[+24]()  /* notifyIpSettings */
    if (checkNetworkConnectionChange())     vptr[+16]()  /* operateChangeNetwork */
} else {                                 ; ---- idle branch ------------------
    if (r3 != 0) { this[+0x24]=1; vptr[+12]() /* operateConnectNetwork */ }
    else if (this[+0x40] && this[+0x44] > 89999) vptr[+12](1)   /* 90 s retry */
}
```

`NetworkMonitor::checkNetworkConnection` `@0x392070` is exactly
`return getPcController() ? getPcController()[+0x72] : 0` — the compiler inlined
it here, which is why it has no `bl` caller of its own. And
`checkNetworkConnectionChange()` `@0x392084` is the `SIOCGIFADDR` `0x8915` /
`SIOCGIFNETMASK` `0x891b` pair on the `eth0` literal (`0x39215c` → `0x3fe678`),
whose result it stores at `[+0x1c]` — **the same field `operateConnectNetwork`
reads as `[+0x24]`**, because the subobject sits eight bytes into the manager and
so is `NetworkManager+0x24`. It returns 0 while the address is unchanged, and it
is called only from the connected branch.

So the whole chain is:

```
getPcController()[+0x72]     ->  this[+0x24] = 1
                             ->  vptr[+12]() = operateConnectNetwork()
    next tick                ->  checkNetworkConnectionChange()
                                   ioctl(SIOCGIFADDR, "eth0")   [netshim: -> wlan0]
                                   NM+0x1c = 192.168.1.239
                             ->  vptr[+16]() = operateChangeNetwork()
                                   -> ProDjLink::setIpAddress / setNetMask
                                   -> ProDjLink::operate(msg +24 = 2 = CONNECT)
```

That byte is **`ui::PcController::isUsbBConnected()`** — `@0x2e6dc8` reads it and
does nothing else — and its only writer in the whole binary is
`ui::PcController::handleUsbMountMessage` `@0x2e9700`: message **3** stores 1
(`0x2e973c`) and message **4** stores 0 (`0x2e9720`). It is, literally, "a PC is
attached over USB-B" — the gate this machine uses to decide it should be on a
network at all.

**This is what the "silent no-op" really was.** `[+0x24]` is zero not because the
substitution failed but because nothing ever sets `[+0x72]`, so the address is
never read. `na_connect_gate()` sees the symptom, correctly, and refuses.

### Three things that are not the gate

* **`-a` is not what stops Link.** The live unit runs `/lib/ld-linux.so.3
  /root/pdj/rbp -a` (read off `/proc/<pid>/cmdline`), and `main` `@0x10274` sets
  `r9 = 1` at `0x10314`, clears it for `-a` at `0x10384`, and passes it to
  `NetworkManager::initialize(bool)` `@0x391860` at `0x1045c`. But that byte only
  gates the Ethernet PHY reset (`gpio -o 2 14 …`) and the `Autoip` construction:
  with or without it, `initialize` still runs `NetworkMonitor::stopMonitoring()`
  → `startMonitoring()` → `getMacAddressUint8()` → `ProDjLink::setSelfInfo()`.
  That accounts for exactly the one `SIOCGIFHWADDR("eth0")` Stage 2 measured. Keep
  `-a`: it is also what leaves `[+0xc0]` NULL, which is why the Autoip branch above
  is never taken.
* **`operateConnectNetwork`'s own `bool` argument is never read.** The Autoip
  branch is gated on `[+0xc4]` instead. Passing 0, 1 or anything else changes
  nothing.
* **`[+0x24]` is a precondition of the *call*, and not the reason there is no
  call.** It is NetworkMonitor's copy of the interface address, written by
  `NetworkMonitor::checkNetworkConnectionChange` `@0x392084` through the `eth0`
  literal — the very field the name substitution exists to make non-zero. While it
  is zero `operateConnectNetwork` returns at `0x38f854`, and firing it by hand is a
  **silent no-op**; that is what `na_connect_gate()` refuses on. But it is zero
  *because* `[+0x72]` is zero — the check that would fill it runs only in the
  connected branch — so it is a symptom wearing the gate's clothes. Getting it
  non-zero by other means (calling the function by hand) still leaves rbp with
  `[+0x24] == 0` on the next tick, which then takes `operateDisconnectNetwork()`.

**The gate itself is `[+0x72]`**, and rbp raises it from its own protocol: the
`EnUsbMountMessage` channel below. That is the lever, and it is in band.

### The trigger: one word on rbp's own USB FIFO

`[+0x72]` is not something a shim has to reach in and poke. rbp already has a
protocol that raises it, and the shim already speaks that protocol — `usb-watch.sh`
writes `mount /media/usb1/sda1` into it on every stick insert.

`ui::UsbMountManager::run()` `@0x320a28` reads `read_sf_rbp` `@0x3785b4` and
broadcasts a fixed `EnUsbMountMessage` to every `ui::UsbMountCallback` registrant.
`ui::PcController` is one of the five. The dispatch table is at `0x320aa0`
(`ldrls pc, [pc, r3, lsl #2]`, base `0x320a98 + 8`), indexed by `event - 1`:

| line written to the FIFO | `read_sf_rbp` returns | `enqueue` broadcasts | reaches |
|---|---|---|---|
| `mount …` | 1 | 5 | |
| `umount …` | 2 | 2 | |
| `do_umount …` | 3 | 1 | (`umount()` on the path) |
| **`connect`** | **4** | **3** | **`PcController+0x72 = 1`** |
| `disconnect` | 5 | 4 | `PcController+0x72 = 0` |

`read_sf_rbp` is a `strncmp` chain — `mov r2,#5/6/9/7/10` against `mount`
(VA 0x4dee34) / `umount` (0x3fe6fc) / `do_umount` (0x4dee30) / `connect` (0x4dee20) /
`disconnect` (0x4dee6c) — setting `r5` to 1…5. So the whole bring-up is:

```sh
printf %s connect > /tmp/udev_usb1      # gate up   -> 3
printf %s disconnect > /tmp/udev_usb1   # gate down -> 4
```

#### Run 2026-10-07: the gate rises, and nothing follows

**This drill was run, and the second arrow does not exist.** Everything up to the
gate was confirmed live on the unit: the word `connect` on `/tmp/udev_usb1` really
does broadcast message 3, and `ui::PcController::isUsbBConnected()` really does
read `1` afterwards — read back out of the running process, on the `PcController`
at `objmgr+0x9c` and again through both `PcChController`s' `+0x54`. The gate goes
up. **Link still does not.**

The cause is not the FIFO and not the shim. `NetworkMonitor::timerCallback`
`@0x392160` reads the gate **through `IUiObjManager::getPcController()`**, and on
this build that getter is stubbed to `mov r0,#0; bx lr` by
[`scripts/patch-rbp-nopc.py`](../scripts/patch-rbp-nopc.py) — the same stage that
stops the startup NULL-deref. With it stubbed, the timer computes `r3 = 0` every
tick, never takes the connect branch at `0x392288`, and never calls
`operateConnectNetwork`. `checkNetworkConnectionChange` is reachable only from
that branch, so `NetworkMonitor`'s IP field (`+0x1c`) stays zero, and the
out-of-band `NETALIAS_CONNECT` route then refuses forever with `no-ip`. **One
2-word patch disabled Pro DJ Link on both routes, silently.**

The stub is gone as of 2026-10-07 (the getters now return NULL instead of
faulting, keeping their stock 5-word shape — see
[PATCHES.md § 11](../tools/patch-rbp/PATCHES.md)). **The drill has not been re-run
on the fixed build**, so the FIFO word is still a *candidate* trigger, not a proven
one.

Fired from the host, writing the same path `usb-watch.sh` writes: rbp opens these
FIFOs `O_RDWR` and holds them (fds 31/32/35/36, seen in `/proc/<pid>/fd` on the
unit), and the chroot's `/tmp` is a **bind mount of the host's** — `stat` on both
spellings of the directory reports the same `st_dev:st_ino` (`35:14`), so the host
path is the path rbp reads. The existing writer sends the line with **no trailing
newline** (`printf "%s" "$argmsg" > "$argfifo"`, `usb-watch.sh:282`) and
`read_sf_rbp` matches on the prefix, so the bare word is what the protocol carries.

**Never write anything else to this FIFO.** An unrecognised line does not return
an error. `read_sf_rbp` tests the five words in order and, when none matches,
branches to `0x3787e0` — which sets `r4 = 0` and joins the common tail at
`0x3786d4` **with `r5` still holding `poll()`'s return**, the count of ready
descriptors. That count is 3 whenever all three polled fds are ready, and the tail
then reads:

```
3786d4   cmp  r5, #3          ; "do_umount"?  -- but on the fall-through r5 is a COUNT
3786d8   bne  return          ; 1 or 2: safe
3786dc   cmp  r4, #1          ; r4 is the digit after "do_umount"; 0 on the fall-through
3786e0   beq  ...
3786e4   mov  r0, r6          ; r6 = the caller's OWN out-buffer
3786e8   bl   umount@plt      ; <-- unmount, on whatever that buffer happens to hold
```

So a junk line is a chance of unmounting with an arbitrary **path**: the buffer was
never written by this call, so its contents are the caller's, not ours. There is no
such thing as a harmless probe on this channel.

`disconnect` is the way back, and it is quick — the next one-second tick sees
`[+0x72] == 0` in the connected branch and runs `operateDisconnectNetwork()`. It is
not a panic button: it stops rbp's own UdpServer, and it does not recall whatever
the `connect` half already announced to the CDJ.

### The other channel, and the one that is NOT a door

`/tmp/udev_usbctn1|2` is a second, independent FIFO pair — same shape, opened by
`open_sf_caution` `@0x38706c` and read by `read_sf_caution` `@0x387160`, consumed by
`UsbCautionManager::run()` `@0x377bcc`. Its grammar is `connect`/`disconnect`/
`huberr`/`unsupported`/`fsck_start`/`fsck_finish` and it broadcasts 0/1/2 only. It
drives the caution popup, and reaches no part of the Link gate. **`connect` on the
caution FIFO is not the trigger** — the same word on two channels does two
unrelated things, and only `udev_usb1|2` moves `[+0x72]`.

### The shim-side trigger, kept

`RB_NETALIAS_CONNECT=1` arms a one-shot direct call into
`NetworkManager::operateConnectNetwork`, gated by `na_connect_gate()` in
`netalias.c` and fired by creating `/tmp/rb_link.req`. It was built and host-tested
before the FIFO route was found, and it is now **redundant** — the FIFO route is in
band, needs no vtable address, and sets the gate the way rbp sets it rather than
bypassing it. It is kept because it reaches the call without an attached-"PC" state
(useful if the gate itself is ever the thing under test) and because deleting a
built route costs more than leaving it disarmed.

Its two refusals are still the honest description of *why* a hand-made call is not
the fix, so they stay pinned in `test_netalias`:

| input | verdict | why |
|---|---|---|
| singleton `0` | `no-singleton` | `main` has not reached `getInstance()` yet. Nothing to call; keep the request |
| address `0` | `no-ip` | `[+0x24]` is zero, so the call returns at `0x38f854`. Firing would be **silent** — refuse and keep the request |
| otherwise | `go` | unlink the request, then call once |

The singleton is read **once** into a local and that same value is used for both the
gate and the call: `main` is running concurrently and may publish it between two
reads. Arming turns the log **on**, because the two outcomes — "it worked" and
"silence" — read the same from outside if nothing is written down.

**If the FIFO route is used, leave `RB_NETALIAS_CONNECT` off** (`0`), so that
whatever happens on the wire has exactly one cause. The substitution
(`RB_NETALIAS=1`) is still required on the FIFO route too: the gate makes rbp read
the address, and the substitution is what makes the address non-zero.

## Verification

**Stage 0 — host. Done.** Four gates, all green:

| gate | result |
|---|---|
| `make test` → `test_netalias` | **110 checks, 0 failures.** Pins every `SIOCSIF*` and `SIOCGIFCONF`/`SIOCGIFNAME` as refused, a non-matching name as passed, all four selection rules including both identity fallbacks, that `udhcpc` is never rewritten, and both refusals of the Link gate plus the order they are asked in. It also asserts every `NA_SIOC*` against `<sys/ioctl.h>`, every flag against `<net/if.h>`, and `sizeof(struct na_ifreq) == sizeof(struct ifreq)` — which caught a wrong guess at the `IFF_*` bits, since these are the kernel's and they are not adjacent |
| `make check` | `netshim.so` shows only `GLIBC_2.4` (no hard-float tag) and **all symbols resolved** against the vendor libc |
| deliberate breaks | three, all verified red: a setter added to the whitelist → 2 failures; `NA_IFF_RUNNING` moved to the adjacent bit → 1 failure; the `udhcpc` refusal weakened to pass → 2 failures |
| `netcheck` (below) | **the shim really rewrites**, measured under `qemu-arm` on the host |

Three deliberate breaks matter more than they look: a whitelist's failure mode is
being too *permissive*, and a test that only ever asserts "the good cases work"
cannot see that at all. Two of the three breaks exist to fail the negative
direction.

### `netcheck` — the shim, not the rules

`test_netalias` tests the rewrite *rules*. `netcheck` tests the *shim*: it is an
`LD_PRELOAD` subject, so the constructor, the `RTLD_NEXT` chain and the name
restore are all exercised, and a rewrite that never happens reads as a wrong
address rather than as a silent pass. Like `ccexit` it must **not** be `-static`
— a static binary has no dynamic loader and `LD_PRELOAD` is ignored — which is why
it is not in `TESTS`.

```sh
# on the unit, inside the chroot
NETALIAS=1 NETALIAS_IFACE=lo NETALIAS_LOG=1 \
    LD_PRELOAD=/usr/lib/netshim.so ./netcheck
```

*Measured on the host*, running the real ARM `netshim.so` under `qemu-arm -L
<rootfs>` with `NETALIAS_IFACE=lo`, against the same drill with no shim:

| line | no shim | with `netshim.so` |
|---|---|---|
| `SIOCGIFADDR eth0` | `172.17.0.3` | **`127.0.0.1`** = the alias's answer |
| `SIOCGIFADDR lo` | `127.0.0.1` | `127.0.0.1` (control: untouched) |
| `SIOCGIFADDR nosuch0` | fails `ENODEV` | fails `ENODEV` (untouched) |
| `if_nametoindex eth0` | `11` | **`1`** = the alias's index |
| `if_nametoindex nosuch0` | `0` | `0` |
| `SIOCSIFADDR eth0` | refused `EPERM` | refused `EPERM`, **not rewritten** |
| `ifr_name` after every call | its own name | its own name — the restore holds |

The drill prints `netcheck: ok` (exit 0) with the shim and `netcheck: FAILURES
above` (exit 1) without it. That is the intended reading: **the drill asserts the
shim is doing its job**, so its baseline run is a failure. The negative cases are
asserted in both directions — a shim that rewrote *every* name it was handed would
pass the first two lines and fail `nosuch0`, which is the only thing that catches
it.

The `ld.so: … wrong ELF class: ELFCLASS32` lines that appear in that run are a
host artifact: `system()`'s child is the **x86-64** `/bin/sh` on this workstation,
which sees an ARM `.so` in `LD_PRELOAD` and ignores it. On the unit both are ARM
and the message cannot occur. `netcheck-system-ran` printing is the proof the
`system` hook delegated correctly.

**Stage 1 — unit, shadow. Run 2026-10-07.** See
[above](#what-rbp-asks-for-and-what-it-does-with-the-answer) for the result:
exactly one `ifreq` request — `SIOCGIFHWADDR` on `eth0`; no fourth call; and
neither `udhcpc` nor the `ifconfig` scrape runs, so `RB_NETALIAS_MAC_SCRAPE` has
nothing to gate and stays `0` permanently.

**Stage 2 — unit, active, manual. The go/no-go. Run 2026-10-07. Signal 1 PASS;
signals 2–4 FAIL.** The log shows the alias armed on `wlan0` and the request
rewritten, then stops. `ss -lunp` shows only `UDP 0.0.0.0:42333` and
`UDP 127.0.0.1:20000` — **no 50000** — and the packet sockets on the unit belong
solely to `wpa_supplicant` and `NetworkManager`. The log froze at 43 lines and
stayed there across a 100 s watch. Signal 2's failure *is* the stop condition the
design named — "rbp is not starting its Link stack at all and no shim will help" —
and it is decisive, but **not for the reason this document first gave.** The
interface substitution is not implicated: rbp never asked. `timerCallback` reads
the Link gate through `IUiObjManager::getPcController()`, which this build has
stubbed to `mov r0,#0`; the gate therefore reads as 0 on every tick, the connect
branch is never taken, and no socket is ever opened. See
[Run 2026-10-07](#run-2026-10-07-the-gate-rises-and-nothing-follows) — the same
stub is why the FIFO drill failed. Stage 2 measured a true negative for a wrong
reason, and it has to be re-run on the fixed build before it can judge the shim.

### Stage 2b — the isolation drill. Run 2026-10-07. The AP does **not** isolate clients

This stage exists because Stage 2's signals 3 and 4 cannot be read as network
evidence once signal 2 has failed: rbp sends nothing, so nothing can come back,
and "no reply" would be an artifact of the silence rather than a fact about the
network. The question was therefore asked directly, with something that *does*
emit. Neither probe is Pro DJ Link:

| probe | result |
|---|---|
| `ping` the CDJ at `.201` — control: the gateway `.1` | **3/3 received**, 2.4–47.9 ms |
| ARP for `.201` | `c8:3d:fc:18:af:77`, **REACHABLE** |

Both directions of station-to-station relaying work: the Pi's broadcast ARP
request reached the CDJ and the CDJ's unicast reply reached the Pi. And that ARP
MAC is **byte-for-byte the MAC inside the CDJ's own keep-alive packet**, an
independent confirmation that this peer is who its packets claim to be.

Then the announce itself. The CDJ's `CDJ-3000` keep-alive, captured off this
network, patched in exactly four places — name → `RB-TEST`, `0x26–0x2b` → the
Pi's `wlan0` MAC, `0x2c–0x2f` → `192.168.1.239`, `0x24` → device number 5 (so as
not to claim the CDJ's own 3) — broadcast to `192.168.1.255:50000` every 1.5 s for
30 s (19 sends) while listening on 50000.

*Result:* the packet went out and looped back to our own listener (x38, two per
send); `.201` and `.59` keep-alived normally throughout; **no reply arrived, and
the CDJ's LINK screen showed nothing.** That is consistent with two different
causes and does not by itself separate them — a bare keep-alive is **ignored by
design**, because a device only joins by running the startup handshake (the
`0x0a` announcement, then the `0x00`/`0x02`/`0x04` channel claims), which happens
only at a device's own power-up and so has no sample on a steady-state network.
What 2b settles is not the packet question but the network one, and the ping and
ARP rows above settle it: **there is no client isolation here**, so
[risk 4](#risks-ranked) is retired.

**Stage 3 — service. Run, then reversed, and now armed again.** `netshim` second in
`RB_LD_PRELOAD`, `RB_NETALIAS=1` in `rb.local.conf`, restart — the state Stage 2
measured. Returned to `RB_NETALIAS=0` on 2026-10-07 once signal 2 had decided the
question; the launcher printed `on=0` and `-> pass (shadow)` again, which is the
read-back that the disarm took. Rollback was one line, exactly as designed.
`rb.local.conf` **currently reads `RB_NETALIAS=1`** (with `RB_NETALIAS_CONNECT=1`
still on from the earlier work), so the substitution is live on the unit right now
and a drill needs no edit before it — only the disarm of `RB_NETALIAS_CONNECT`
below, so the cause of anything on the wire is single.

**Stage 4 — the bring-up. RUN 2026-10-07, FAILED, then re-opened.** The sequence
below was executed on the unit and the second `ss` stayed empty. The failure was in
rbp, not in the shim or the write: the gate went to 1 exactly as the table's first
row predicts, and nothing followed it, because `getPcController()` is stubbed and
`timerCallback` cannot see the gate at all. **This stage is a valid test again only
on the fixed build** — see
[Run 2026-10-07](#run-2026-10-07-the-gate-rises-and-nothing-follows). One write and
one look:

```sh
# rb.local.conf:  RB_NETALIAS=1   RB_NETALIAS_CONNECT=0     (the second is a disarm)
# restart, then, on the HOST, watching the glass and a second shell:
ss -lunp | grep :50000
printf %s connect > /tmp/udev_usb1
sleep 3
ss -lunp | grep :50000
```

| reading | means |
|---|---|
| `ss` before the write shows nothing, after shows rbp on **UDP 50000** | **the Link stack is up**, and it went up because rbp's own gate was set — not because a shim reached past it |
| `ss` still shows nothing | read the log; if `alias eth0 -> wlan0` is absent the substitution did not take (Stage 2), and if the gate is still zero the FIFO write did not land |
| rbp holds **UDP 50000** | remaining question is whether a peer answers — a capture on `wlan0` for the 54-byte `Qspt1WmJOL` discovery packets settles it, but so does the CDJ-3000's own LINK screen |

`printf %s connect` and nothing else: **no newline is needed and no other word is
legal.** See the junk-line warning above — an unrecognised line can reach
`umount(2)`.

**Off-air only** ([risk 3](#risks-ranked)): sources loaded, nothing playing, SYNC
off, with the operator able to see the CDJ's LINK screen. Rollback: `printf %s
disconnect > /tmp/udev_usb1` (one tick), and `RB_NETALIAS=0` + restart to take the
substitution out entirely. Nothing is written to disk by either half.

## The log

`NETALIAS_LOG=1` writes `/tmp/netshim.log`, cleared by the launcher before each
launch. The armed line names the decision; each request is logged **once per
distinct code/name pair**, so a hot path does not fill a 1.9 GB tmpfs the way
`RB_VERBOSE=1` does ([12](12-troubleshooting.md)):

```
NETALIAS v1 pid=14 armed log=/tmp/netshim.log on=1 mac_scrape=0 alias=lo src=env \
    real=ioctl:ok,if_nametoindex:ok,system:ok
NETALIAS v1 pid=14 ioctl req=0x8915 name="eth0" -> rewritten
NETALIAS v1 pid=14 ioctl req=0x8916 (not an ifreq) -> pass
NETALIAS v1 pid=14 system shadow "echo ..." -> pass
```

`real=ioctl:ok,…` is the check that all three `RTLD_NEXT` resolutions succeeded —
if one reads `FAIL`, that call is being handled without a real implementation
behind it and the run is not evidence. `alias=` with `src=env` versus `src=auto`
says whether `NETALIAS_IFACE` or the four rules chose, and `src=none` means every
hook is passing through.

## Risks, ranked

1. **A setter ioctl retargeted at `wlan0`** — would drop the operator's SSH and
   the unit's LAN. Prevented by the whitelist, and pinned in `test_netalias`
   first. This is the only catastrophic-if-it-happened failure here.
2. **The `ioctl` order collision with `fbshim`** — silent partial failure one way,
   a broken display the other. Mitigated by the `RTLD_NEXT` chain and the
   `doctor.sh` order check.
3. **Joining the live Link network disturbs the CDJ-3000.** Today rbp is silent on
   the LAN; this is what makes it audible. rbp would announce itself, take a
   device number and appear in the CDJ's LINK list, and if it plays a deck it
   broadcasts tempo and can become a sync reference a CDJ in `SYNC` would follow.
   **Test off-air only** — sources loaded, nothing playing, `SYNC` off, one deck,
   with the operator watching the CDJ's LINK screen. Never during a set.
4. ~~**AP client isolation.**~~ **Retired 2026-10-07 by Stage 2b.** It was the
   one risk that would have made every other question moot, and it is not present
   on this network: ping and ARP both reach the CDJ and come back. Kept in this
   list because the *test* is the reusable part — a negative result from a silent
   sender proves nothing, so the drill had to emit on its own rather than wait
   for rbp to.
5. **The `system` hook running DHCP on `wlan0`.** Prevented by design — never
   rewritten, and the whole hook is observing-only — and still pinned in the test
   suite.
6. **`getifaddrs`/`dlsym` failing in the constructor** — inert by rule 4, never a
   NULL dereference.
7. **MAC inconsistency** between `SIOCGIFHWADDR` (answered by `wlan0`) and the
   un-rewritten `ifconfig` scrape (still `eth0`). Low severity; the Stage-1 log
   says whether the scrape even runs.
8. **Calling into rbp at an absolute address.** `0x38f830` is valid because the
   binary is non-PIE *and* byte-identical to stock v1.20 — the same fact the meter
   hook at `0x2d07a8` already relies on. If rbp is ever replaced by a build that
   is not that binary, this address and the four in `rbp_abi.h` become meaningless
   and the call would jump into whatever now lives there. Everything else in
   netshim degrades to a no-op in that world; this one does not. The mitigation is
   the gate, not a check: it will not fire on a player that has no network address,
   and it is off by default. **The FIFO route needs none of this** — it uses a path
   and five words, both of which the player itself defines — which is the second
   reason it is the better door.

## Not in scope

* **Beatport or any streaming service.** rbp has no client at all; zero strings.
* **Wireless-name handling beyond the substitution** — nothing else reads `wl*`.
* The `rbp-no-prodjlink-no-wireless` note in the project's memory, whose
  "discarded workstream" verdict this reverses. What that note recorded is still
  true and still worth keeping: the binary's network layer is twelve functions
  hardcoded to the literal `eth0`, and the wireless names are signatures only.
  What it concluded from that — that there was no route — is what changed; the
  route is a name substitution at those three call sites, not new networking code.

## Open measurements

* ~~Whether the twelve functions issue the four predicted ioctl codes, and
  whether a **fourth** call exists.~~ **Answered (Stage 1):** one call,
  `SIOCGIFHWADDR`, and no fourth.
* ~~Whether `udhcpc` and the `ifconfig` scrape run at all in the vendor rootfs.~~
  **Answered (Stage 1):** neither does. `RB_NETALIAS_MAC_SCRAPE` stays `0`
  permanently.
* ~~Whether rbp starts its Link stack once it believes it has an address, and
  whether the AP passes station-to-station traffic.~~ **Answered (Stage 2 and
  2b):** it does not, and the AP does.
* ~~**What would still have to be true for rbp to show LINK:** something *inside*
  rbp must post the network-up command into the `NetworkManager` pump. That is
  internal reverse engineering of `EnNetworkCommandType` and the queue, and no
  network-side library helps with it.~~ **Answered (re, 2026-10-07):** no
  `EnNetworkCommandType` and no synthetic `CommonMessage` is needed, and no shim
  call either. It is not the queue: it is one **byte**, `ui::PcController::`
  `isUsbBConnected()` (`PcController+0x72`), read by
  `NetworkMonitor::timerCallback` `@0x392160` once a second from `main`'s own
  `initialize()`, and set by `EnUsbMountMessage 3`. **rbp raises that message
  itself** when the word `connect` is written to `/tmp/udev_usb1` — see
  [The gate, and the call behind it](#the-gate-and-the-call-behind-it). The
  correction matters: an earlier reading of this page said
  `operateConnectNetwork` had no caller and that a shim therefore had to call it.
  The caller is rbp's own one-second timer. What remains **open** is whether
  setting the gate makes LINK appear: one FIFO write and one look at
  `ss -lunp | grep :50000`, and it has not been run.
* The small caption **under the drive icon** on the SOURCE screen is unrelated to
  this — it is the open `USB1`/`USB2` item in [10 — USB](10-usb.md).
