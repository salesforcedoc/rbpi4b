# 19 — `vncserve`: rbp's screen over VNC, with a raw ↔ hwjpeg switch

`vncserve` serves **rbp's screen** — the whole screen, drawers and all — to any VNC
client, and lets the operator switch, at run time, between

* **`raw`** — the framebuffer's own pixels, unchanged, one rect; and
* **`hwjpeg`** — the same frame put through the Pi's **hardware JPEG encoder**
  (`/dev/video11`) and sent as a Tight-JPEG rect.

**The two modes are not two settings of one dial, and `hwjpeg` is not always reachable.**
The RFB specification permits a server to send JPEG only to a client that has asked for a
JPEG *quality level*, and macOS's Screen Sharing never does — so a Screen Sharing session
is a `raw` session whatever the mode says, and the hardware JPEG is something to look at
on the control page's preview instead. Read [the rule](#the-rule-that-keeps-jpeg-out-of-a-screen-sharing-session)
before drawing conclusions from the mode line.

The switch is a **tiny web page** served by the same program, so the operator can
see the mode, change it, and *look at the result* without touching the VNC session
they are watching.

The chosen client is **macOS's built-in Screen Sharing** (`vnc://192.168.1.239`) —
zero install. That choice is not cosmetic: it is what fixes the handshake dialect
(below), and it is why security type *None* is not offered at all.

Source: [`scripts/device/vncserve/`](../scripts/device/vncserve/) — about 4 300 lines of
C plus 810 lines of tests, needing **only libc**.

## Why it is hand-rolled

`libvncserver` cannot do the hardware JPEG. Its Tight encoder produces its own JPEG
*from the framebuffer*; there is no way to hand it a JPEG that is already encoded —
and the hardware encoder is the entire point of the feature. So the RFB server is a
small, fully-specified subset: handshake, `ServerInit`, `SetPixelFormat`,
`SetEncodings`, `FramebufferUpdateRequest`, and the two encodings above. Nothing
else. No package has to be added to the unit.

## The measured facts this rests on

| fact | value | how |
|---|---|---|
| `/dev/fb0` | `vc4drmfb`, 1280×800, 16 bpp **RGB565**, stride 2560 B | the only format vc4 offers (see [06](06-display.md)) |
| fb0 full read | **9.5 ms/frame** | mmap, no `read()` |
| **half the screen is not in fb0** | the two edge drawers, the top band and the USB-STOP chooser are **vc4 DRM overlay planes** on `/dev/dri/card1`, RGB565, **opaque** | [06](06-display.md) names this gap |
| `/dev/video11` | `bcm2835-codec-encode`, a V4L2 **M2M** device; accepts `RGB565` (`'RGBP'`), emits `MJPG` | `--list-formats-out` / `--list-formats` |
| the encoder's speed | **10.73 ms/frame, 93.2 fps** at 1280×800; 16–37 KB a frame | 30 consecutive frames, 322 ms total |
| its quality knob | **there is none on this node** | `V4L2_CID_JPEG_COMPRESSION_QUALITY` is refused **here**; the device exposes the H.264 control set instead |
| `/dev/video31` | `bcm2835-codec-encode_image`, the **image** encoder — a different component. Also takes `'RGBP'`, but emits **`JPEG`** where the video encoder says `MJPG`: the same bitstream under two names | `--list-formats-out` |
| **its quality knob** | **`compression_quality`, 1–100, default 80** — the control the row above says is refused, refused *there* | `v4l2-ctl -d /dev/video31 -l` |
| video31's speed | **15.5 ms/frame, 60–65 fps** at 1280×800, against a 12 fps budget | `work/venc_bench.c`, 120 frames from the live screen |
| **what the two cost** | for the same screen: video11 ≈ **45 KB**; video31 **182 KB at 80**, 132 at 60, 104 at 40, 72 at 20 — and **61 KB at 12**, measured through the server | so *matching* video11's bytes is about **quality 6–8**, not 10–15 |
| macOS Screen Sharing's `SetEncodings` | **thirteen entries**, led by `zlib` (6) and `ZRLE` (16), and **no Tight and no Raw**; and **no JPEG quality level** | the log of a live session, 2026-10-09 11:52 — see below |
| the same, as this repo previously recorded it | `Raw, Tight, NewFBSize` — three entries | **wrong, and it cost a day.** That is *this repo's own test client's* list (`rfbclient.py`'s `encs`), mistaken for Apple's |

`V4L2_PIX_FMT_RGB565` **is** `'RGBP'` (`videodev2.h:546`), so the driver's echo is not
a substitution and no byte-swap is needed — verified by pushing colour bars through
and getting red, green and blue back.

## The cost model, measured with one client at 4 fps

| mode | bandwidth | CPU (one core) | bytes a frame |
|---|---|---|---|
| idle (no client) | — | 0.17 % | — |
| **raw** | **57.3 Mbit/s** | 4.50 % | 2048 KB |
| **hwjpeg** | **1.3 Mbit/s** | 3.83 % | 41.2 KB |
| **zlib (encoding 6)**, the real client | **1.73 Mbit/s** | 8 % | 52.8 KB an update, 5–6 rects |

**Read the third column sceptically: the CPU is nearly the same.** At 4 fps the
capture dominates, and a 2 MB socket write costs about what the encoder does. So the
win is **bandwidth — 44× — not CPU**. That is still the whole point on this unit,
which is on **`wlan0`** (`eth0` is down): 57 Mbit/s will not fit through the WiFi the
decks are playing over, and 1.3 Mbit/s will.

The last row is the one that matters, because it is the **only** row that was ever
serving the operator. It is also the only one with no encoder in it: `zlib` is lossless,
CPU-only, and — unlike `hwjpeg` — it is an encoding macOS's client actually asks for.
**The `raw` row is not the fallback for a Screen Sharing session; it was the default.**
See "Encoding 6" below.

`RB_VNC_FPS` in `rb.conf` carries this table as its comment. 4 fps is the default.

## The capture, and the half of the screen that is not in fb0

```
vnc_capture_frame(dst)                 /* dst is w*h RGB565 */
  1. memcpy fb0's mapping into dst                          (mmap, no read syscall)
  2. enumerate card1's planes, at most every 250 ms
  3. for each plane with crtc_id != 0 and fb_id != 0:
        map its fb (GETFB -> MAP_DUMB -> mmap), cached by fb_id
        vnc_compose_blit(dst, dst_w, dst_h, plane)
```

A viewer that reads `/dev/fb0` alone shows a screen with **no drawers and no top
band** — which is exactly what the first screenshots of this port looked like. Three
things were learned getting past that:

* `DRM_CLIENT_CAP_UNIVERSAL_PLANES` must be set **once**, or the kernel lists 48 of
  card1's 60 objects and hides every primary and cursor plane.
* `GETPLANE` wants a format array no larger than the real count (39) or it returns
  `EINVAL`. It is sized at 64 and the true count read back.
* **Plane geometry is not readable through the property API on this device.** vc4 is
  not atomic; the placement properties are `DRM_MODE_PROP_ATOMIC` and the kernel omits
  them. Geometry comes from **`/sys/kernel/debug/dri/1/state`** instead.

The plane's **pixels** are re-read every frame — the drawer is repainted in place —
and only the *mapping* is cached. Planes are single-buffered, so a read that lands
mid-paint can tear; the next frame is clean, and this is cosmetic.

**Composite is by copy, never blend**: an RGB565 overlay plane is opaque.

## The three wire-format details that had to be settled first

### The handshake dialect

RFB has two handshake shapes and **the session takes the lower of the two
versions**:

| | 3.3 | 3.7 / 3.8 |
|---|---|---|
| server's version | 12 bytes, e.g. `RFB 003.008\n` | same |
| security | **one `uint32` type, no choice** | `uint8` count + `uint8 types[]`, client picks one |
| `SecurityResult` after **VNC auth** | **4 bytes — yes** | 4 bytes |
| `SecurityResult` after **None** | **none** | 3.8 yes, 3.7 no |

`vncserve` announces `RFB 003.008`. **macOS's Screen Sharing answers `RFB 003.003`**,
so the session that follows is a **3.3** session: a single named security type and no
list. Both sides speak 3.8; the rule makes them speak 3.3.

**The `SecurityResult` row is split, and getting it wrong is not an error — it is a
hang.** It is tempting to summarise 3.3 as "no `SecurityResult`", and that summary is
half true: it is what RFC 6143 appendix A.1 says about the **`None`** path, which
"proceeds directly to the initialization messages". For **VNC Authentication the word
is sent in every version** (7.1.3, and A.1/A.2 for 3.3/3.7 alike); only the reason
*string* that 3.8 appends on failure is version-specific. This server had it as one
blanket rule for 3.3, so a 3.3 session that accepted a password simply stopped talking
at the point the client expected to be told it had worked. **The client's reaction is
to wait, not to complain**: macOS's Screen Sharing sat on the challenge until the
server's own fifteen-second `ClientInit` timeout fired. On the glass that is a
connection that *spins forever*, and in the log it is a `password accepted` line
followed by silence — which is the least debuggable shape a protocol bug can take.
The rule now lives in one tested predicate, `vnc_rfb_sends_security_result()`.

The `SecurityResult` is also why the 3.3 branch of `--probe` sends a challenge at all:
it used to name the type, skip the exchange the type consists of, skip the word, and
then report the client's silence as "no ClientInit within 5s" — blaming the client for
a hang the probe had caused, on the one path macOS's client actually takes.

**Security type *None* is not offered.** It is the obvious choice for a LAN appliance
and it is the one answer known to close Apple's socket — Screen Sharing refuses a
server that asks for no password at all, and its error does not mention passwords.
So the only type offered is **2 (VNC auth)**: a 16-byte challenge from `/dev/urandom`,
and the client returns `DES-ECB(challenge)` under a key made from the password with
**each byte's bits reversed** — DES key bytes carry a parity bit in the top position;
VNC passwords use all eight, so every byte is bit-reversed before use.

*An empty `RB_PASSWORD` is therefore not "open", it is "does not work"* — and for the
configuration page, which **shares this one credential**, an empty password is the opposite
failure: it refuses writes rather than permitting them.
`vnc-run.sh` says so loudly at startup and `doctor.sh` fails on it.

**`rb.conf` ships the placeholder `password`, so that flipping `RB_VNC=1` gives a
working viewer rather than a client that fails for an unrelated-looking reason.** That
is a deliberate, temporary default and it is guarded the only way a default can be:
`doctor.sh` **warns while it is still the placeholder**, so a unit that is about to be
left somewhere untrusted says so out loud. Two things keep it survivable meanwhile —
the unit **ships disabled**, so on a machine nobody opted in on the value is never read
and no socket is opened; and the password travels on the launcher's command line
(`vnc-run.sh` execs the binary with `--password`), so it is readable in
`/proc/<pid>/cmdline` by anything that can read `/proc`. It is a placeholder, not a
secret.

The expected response is computed **when the challenge is sent**, not when the reply
arrives, and the comparison is one `memcmp` over 16 bytes — so there is no timing
signal to read even though DES is cheap enough to brute-force offline.

Apple's *own* schemes are 30/33/35, Diffie-Hellman based, and are not implemented; a
client that asks for one is dropped with a log line naming it.

### The rule that keeps JPEG out of a Screen Sharing session

The RFB specification's Tight section carries one sentence that decides this whole
feature's shape:

> **JpegCompression may only be used when bits-per-pixel is either 16 or 32 and the
> client has advertized a quality level using the JPEG Quality Level Pseudo-encoding.**

Advertising **Tight** is a claim about the *container* — I can decode zlib-compressed
rectangles in this framing. Advertising a **quality level** (`-32` … `-23`, or the
fine-grained ladder `-512` … `-412`) is a separate and much stronger statement: *I accept
a **lossy** payload.* The spec makes the second one a precondition for JPEG, and a server
that sends JPEG to a client which never sent it is not being generous, it is violating the
protocol.

**macOS's Screen Sharing sends no quality level, and — this is the part that was wrong for
a day — no Tight either.** Its whole `SetEncodings` is thirteen entries, and Tight is not
among them:

```
1011  1002  6 (zlib)  16 (ZRLE)  -239 (RichCursor)
1104  1100  -223 (NewFBSize)  1101  1105  1107  1109  1110
```

The first four are the ones that matter and the rest are Apple's own pseudo-encoding range.
Note that **Raw (0) is not in the list either**, so this server was not even falling back to
an encoding the client had offered; it was inventing one.

This server ignored that rule, on the theory that Tight was the only interesting question. The
client's reaction was immediate and total: it authenticated, took `ServerInit`, asked for a
frame, *received the JPEG*, and closed the socket **16 ms later**. macOS then put up the
only thing it had to say about it —

> Make sure Screen Sharing or Remote Management (in the Sharing section of System
> Settings) is enabled on the remote computer.

— which names the wrong machine, the wrong setting and the wrong cause, and is the reason
this took a session log to diagnose rather than a guess.

So the JPEG path is gated on `client_jpeg_ok()`, which requires **both** flags, and the
`SetEncodings` log line now says so in as many words when they disagree. The consequence is
worth stating plainly because it will not change: **there is no way to put the hardware JPEG
through a Screen Sharing session.** A JPEG has exactly one container in RFB — a Tight
rectangle — and Apple's client does not offer Tight, so it fails both halves of the gate
independently. A client that does ask for a quality level — TigerVNC's viewer with its JPEG
option on, or the scripted client in the probes below — still gets the full hardware-JPEG
path, and the control page's preview always shows it.

### Tight-JPEG framing

Read out of libvncserver's encoder, decoder and header, a Tight rect carrying a JPEG
is, byte for byte:

```
0x90                 compression-control byte = rfbTightJpeg << 4. The decoder shifts the
                     low nibble off first (four stream flags), leaving 0x09, which it then
                     compares BY EQUALITY against rfbTightJpeg — so 0x90, exactly.
<compact length>    1-3 bytes: 7 bits each, LSB first, bit 7 = "another byte follows";
                     the THIRD byte carries a full 8 bits (& 0xFF, then << 14), so the
                     encoding is only 7-bit for the first two.
<JPEG bytes>         exactly that many
```

A 40 000-byte JPEG goes out as `90 C0 B8 02 …`. No zlib, no palette, no filter byte —
this is the "basic compression" form, and it is what a client that advertised
`rfbEncodingTight` will decode.

**Pinned in `test_vnc_rfb.c`**, because getting it wrong is a silent protocol desync
rather than an error.

Framing the bytes correctly is necessary and not sufficient. The rule above is checked
first, and a client that has not asked for a JPEG is never sent one of these at all — it
gets a Raw rect, which every client can decode.

### Encoding 6 — `zlib`, the one that was missing

**This is the section that made a Screen Sharing session usable, and it exists because the
encoding the client actually asks for was not implemented.** The whole of the rest of this
document was built on the assumption that Tight was the encoding that mattered. For macOS
it is not: Tight is not in its list at all, so `client_jpeg_ok()` was false *and* the Tight
arm was unreachable, and every real session took the Raw floor — 4 096 016 bytes an update,
with the socket's send queue sitting at **1 528 712 bytes**. The operator's report was
"it seems like it is faster refreshing on 5902 webpage than it is actually in vnc", and that
is exactly what 4 MB a frame looks like from the outside.

A **zlib** rectangle is a **Tight zlib rectangle with the wrapper removed**:

```
<12-byte rect header with encoding = 6>
<uint32 length, BIG-ENDIAN>    the byte count of the compressed data only
<zlib bytes>                   exactly that many
```

No control byte, no compact length, no palette, no filter byte. Everything underneath is
identical, which is why `send_comp_update()` in `vnc_session.c` writes both encodings from
one function, with a four-line branch for the header and nothing else differing:

| | Tight (7) | zlib (6) |
|---|---|---|
| header before the chunk | `0x00` + compact length (2–4 B) | `uint32` big-endian (4 B) |
| the deflate stream | one per connection, never reset | **the same** |
| flush | `Z_SYNC_FLUSH` per rectangle | **the same** |
| the bands | the same row bands | **the same** |
| the pixels | the client's format, row by row | **the same** |

**RFC 6143 does not specify this encoding either.** It lists encoding 6 as a historic IANA
assignment and stops, exactly as it does for Tight. The authority is libvncserver, which
both writes and reads it — `hdr.nBytes = Swap32IfLE(cl->afterEncBufLen)` in
`src/libvncserver/zlib.c`, and `rfbClientSwap32IfLE(hdr.nBytes)` in `src/libvncclient/zlib.c`,
each behind a stream flag (`compStreamInited` / `decompStreamInited`) that is set once and
never cleared. The big-endian length is the load-bearing detail: read little-endian and a
40 000-byte rectangle decodes as a 1084-megabyte one.

**Pinned in `test_vnc_rfb.c`** alongside the Tight framing, including the two field bounds
(0 and `0xFFFFFFFF`) and the fact that the two encodings' first bytes are *both* `0x00` for
any length under 16 MB — so it is the rectangle header's encoding field, and never the
payload, that a client dispatches on.

Measured on the unit, one client at the 4 fps ceiling, `--encodings apple` (the real list
above) and a 32-bpp client format:

| | bytes | |
|---|---|---|
| first frame | **128 689** | one rect, the whole screen, `zlib` |
| each incremental update | **52.8 KB**, 5–6 rects | |
| rate | **82 updates in 20 s = 4.1 fps** | the server's ceiling, not the client's |
| bandwidth | **1.73 Mbit/s** | 33× less than Raw |
| socket send queue | **0** | was 1 528 712 |
| decoded picture | `1280×800`, correct | an independent inflate in `rfbclient.py` |

### The lesson worth keeping

The list in the table at the top of this document was wrong for a day, and the way it was
wrong is the interesting part. It was not a guess: it was *a reading of a real log line*,
attributed to the wrong process. `rfbclient.py` sends exactly `[0, 7, -223]`, and every
three-entry session in the log was that script. The attribution cost nothing to check —
`lsof -nP -iTCP` on the Mac names the process holding the socket — and it was not checked
until the operator asked why the picture was slow.

Two changes came out of it, both cheap:

- the `SetEncodings` log line now prints **the raw numbers as well as the names**, because
  a name that is quietly wrong reads as an answer while an unnamed number reads as a
  question;
- `rfbclient.py` grew `--encodings raw|tight|apple`, and `apple` is now **the list read off
  the wire**, so no future test can accidentally validate the server against its own
  assumption.

## The switch

`/run/rblive4/vnc.mode` holds `raw` or `hwjpeg`. It is re-read every frame, mtime-gated
so the common case is one `stat`. It is the project's own idiom — the same shape as
`/tmp/udev_usb1`.

The parsing is the part worth a test (`test_vnc_mode.c`, 31 checks): blanks and case
are forgiven (`"  HWJPEG \r\n"` is `hwjpeg`), and **everything else is refused** —
`jpeg`, `hwjpegx`, `draw`, `0`, an empty file — because a control file that says
something else must not be read as the nearest match. Anything that is not `hwjpeg`
reads as `raw`, so a corrupt value cannot leave the server in a mode it has no encoder
for.

The **web page** on `RB_VNC_HTTP_PORT` (default: `RB_VNC_PORT` + 1):

* `GET /` — the live mode, whether the encoder is up, how many clients are attached
  and whether any of them advertised Tight, the two buttons, and
  `<img src="/preview.mjpg">`.
* `GET /mode?set=raw|hwjpeg` — writes the file through the *same* parser, then `303`
  back to `/`.
* `GET /preview.mjpg` — `multipart/x-mixed-replace`, fed by the same single encode.

The preview is **not** the VNC mode. It always shows what `hwjpeg` would produce, so
the JPEG can be judged without switching the shared screen — which is the point, and
is what makes the page useful even if Screen Sharing turns out never to advertise
Tight. Preview frames **drop rather than queue** when a watcher falls behind.

### The full-screen view, which is the state the page opens in

There is **one page and one port**, and it loads **full screen**: every word hidden — the
heading, the status table, the buttons, the notes — leaving the picture alone on black,
edge to edge, sized to the window. Tapping the picture brings the words back; tapping it
again hides them. The class `only` goes on `<body>` and the CSS does the rest.

It opens that way on purpose. The page exists to be *looked at* — it is the hardware
encoder's output and the deck underneath it — and the words are what you want second.

This was a **second listener** (`RB_VNC_PREVIEW_PORT`, `:5903`) serving the same preview
with no chrome on it, and it is gone: the operator asked for one URL to remember, and a
state is easier to reach than a port. The reasons it was a port — that a path would still
live inside the control page's scrollable document, and that the picture should own the
whole window — are both satisfied by the tapped state, and one of them is satisfied better,
because the page is already open when the picture appears.

Both states letterbox on black rather than stretching: the screen is 1280×800 and the
window will not be, and stretching makes the deck's circles ellipses.

#### Why the picture's box is not the `<img>`, and why that is the flash fix

The operator reported the picture **flashing**, first on the bare page and then on the
control page too. The second report retired the first explanation: the bare page was said
to upscale to the window and resample, but the control page is capped at 760 px, only ever
downscales, and flashed anyway.

**The server is not a candidate, and that is measured rather than argued.** Re-sampled
2026-10-09, 60 consecutive parts off the wire: every one a complete JPEG, all exactly
1280×800; sizes 45.4–46.7 KB; per-frame mean luma **38.57–39.14**, a spread of 0.57 levels
out of 255; the fraction of near-black pixels constant to a tenth of a percent; gaps a
median 83.6 ms, i.e. 12.1 fps. A contact sheet of twelve consecutive frames shows twelve
complete deck screens with no band, no tear and no black frame. There is **no auto-contrast,
no auto-gain and no adaptive quantisation** anywhere in the path — `vnc_jpeg.c` issues
`S_FMT` and *no* `V4L2_CID_*` control at all — so the hardware encoder is handed RGB565 and
asked for MJPG and nothing else. Nothing in the image pulses.

**What is left is the carrier.** An `<img>` fed by `multipart/x-mixed-replace` is replaced
in place, and browsers clear the old frame before the new one is ready to paint — WebKit and
Gecko both have this recorded, and Gecko reimplemented its multipart handling over it. For
that instant the element **has no frame**, and a `<img>` with no frame is no longer a
*replaced* element, so `width`, `height` and `aspect-ratio` all stop applying to it.
Measured: the box goes **762×477 → 98×25**, with `aspect-ratio` computing to `auto`.

**The first fix was `display:block` plus a definite `aspect-ratio` on the image. It was not
enough, and the operator was right to keep saying so.** The sizing properties do apply to a
block box whatever its content, and the box did hold at 762×477 with the frame destroyed —
but it still left `height` *derived from the image*. A browser that drops `aspect-ratio` in
that instant falls back to `height:auto`, which with no content is **zero**: the picture
collapses vertically and every element below it jumps up and back, twelve times a second.
That is a strobe of the **layout**, and no statistic over the picture's own pixels can see
it — which is exactly why a fix that measured clean did not satisfy the person looking at
the screen.

**The fix is to take the ratio off the image entirely.** The ratio lives on a plain
`<div class="shot">` and the `<img>` merely fills it:

```css
.shot{display:block;width:100%;aspect-ratio:1280/800;background:#000;
      border:1px solid #3a3f45;border-radius:6px;overflow:hidden}
img{display:block;width:100%;height:100%;object-fit:contain;border:0}
body.only .shot{flex:none;width:min(1280px,100vw,160vh);
                height:min(800px,62.5vw,100vh);aspect-ratio:auto;border:0}
```

A div's width never depends on any image, so its ratio always resolves and the box is fixed
before a single byte is decoded. Re-measured with the frame destroyed on purpose
(`naturalWidth` 0): the box holds at **762×477, top 456, bottom 933**, and the note below it
holds at **top 953** — in the tapped state too, at 1280×800 between 50 and 850. Nothing
moves. `overflow:hidden` and `background:#000` mean the gap paints black inside a box that
has not moved.

**A caution about the instrument.** While chasing this, a screencast of the rendered page
was recorded as JPEG and its frames alternated in brightness every frame — which looked
exactly like a strobe, and was the **screencast's own JPEG encoder**, not the page. The same
capture as lossless PNG is flat to 0.12 levels. The page's decoded pixels, read back through
a canvas, never vary by more than 0.12 either. An instrument that shares the pipeline it is
measuring cannot falsify it; see `rbp-prodjlink-*` for the last time this lesson cost a day.

The tap toggle is the **one piece of script** in this program, and it is the smallest kind:
a class toggle on this document. Nothing is fetched and nothing is retried, so the rule the
rest of the page follows still holds — a server restart kills the `<img>` until the page is
reloaded by hand.

**The page is the only optional listener.** The RFB port is mandatory — a session that
is silently absent is worse than one that refuses to start. The page's is not: if it cannot
take its port the server logs `http: … -- the control page is NOT available` and serves the
session anyway, because losing the mode switch over a convenience port would be the wrong
trade. The page also counts as a **viewer** in exactly the way a VNC client does, so opening
it starts the encoder and closing it stops the encoder — one encode, two consumers.

## The build, and why it happens on the unit

There is **no toolchain in this tree that can produce this binary**. The
`rblive4-build` image is soft-float **armel**; the unit's userland is hard-float
**armhf**, and the image has no `arm-linux-gnueabihf-gcc` and no hard-float `libc.a`.
`make all` there would exit 0 having built nothing.

So `install.sh` copies `scripts/device/vncserve/` to `$DEPLOY/vncserve/` and compiles
it **there, with the unit's own gcc**, into `$DEPLOY/.vnc-build.$$`. Two details:

* **A compile failure aborts the install** with the compiler's own output — not a
  summary of it. `install_artifact`'s "no source → return 0 in silence" shape is
  exactly how a deploy can look clean and ship nothing.
* **It does not build in place.** `cc -o vncserve` truncates its output file, and a
  running `rblive4-vnc` has that binary mapped — the truncation would be a `SIGBUS`
  in the operator's live view. Only the finished binary is **renamed** into
  `$DEPLOY/vncserve/vncserve`. (Same trap as replacing any mapped `.so`; see the note
  in `install.sh`'s artifact section.)

`-D_FILE_OFFSET_BITS=64` is load-bearing: dumb-buffer mmap offsets start at 4 GiB,
and a 32-bit `off_t` truncates them to 0. The plane maps would then silently read the
wrong memory — which looks like a capture bug and is not.

On the workstation, `make -C scripts/device` builds a copy with the **host** compiler
(it deliberately ignores `CROSS`) purely so a compile error surfaces here rather than
at install time, and `make -C scripts/device test` runs the pure-module suites
natively: **`test_vnc_compose` 90, `test_vnc_des` 11, `test_vnc_diff` 24,
`test_vnc_mode` 31, `test_vnc_rfb` 108, `test_vnc_zlib` 231 — 495 checks.** That is a
deliberate difference from `scripts/shims/`, whose tests are soft-float armel under
`qemu`. `test_vnc_zlib` needs zlib's headers, which the unit does not carry, so it is
the one suite that cannot run there.

## The service

`rblive4-vnc.service` runs `vnc-run.sh`, which sources `lib.sh` + `rb.conf` and execs
the binary with the `RB_VNC_*` values. It is a unit of its own rather than a branch in
`start-rb.sh`, for two reasons:

1. **A crash in the viewer must not take the player with it.** Under `rblive4.service`
   a failed `vncserve` would be a failed rbp, and the operator's decks would go silent
   because a status page threw.
2. **`RB_VNC=0` must mean the process is not started at all**, and costs exactly
   nothing — which an uninstalled unit gives more cleanly than a branch.

It has **no ordering dependency on `rblive4.service`**, deliberately. It reads the
framebuffer and the DRM planes; with the player down it serves an honest black screen
with the drawers on it, which is a useful thing to look at while working out why the
player is down. An `After=` would also mean a player restart dragged the VNC session
down with it, cutting off the view at exactly the moment it is wanted.

It is **installed only when the binary exists** — `vnc-run.sh` exits 1 on a missing
binary by design, and a unit with `Restart=always` pointed at that would restart-loop
every ten seconds forever.

## Configuration

All in `rb.conf` (the shipped defaults) or, for this unit, `rb.local.conf`
(**never overwritten** — see [scripts/device/README.md](../scripts/device/README.md)):

| variable | default | meaning |
|---|---|---|
| `RB_VNC` | `0` | `1` installs the unit **and enables it**; `0` installs it disabled |
| `RB_VNC_PORT` | `5900` | the RFB port |
| `RB_VNC_HTTP_PORT` | `RB_VNC_PORT + 1` | the control page — the only page; it carries the mode switch and the live preview, and it **opens in the full-screen view** (tap the picture for the words). `0` turns the page off entirely |
| `RB_PASSWORD` | `password` (**placeholder**) | one credential for the viewer **and** the configuration page; **worth changing**; empty means macOS will not connect at all. `RB_VNC_PASSWORD` is the same credential under its old name and derives from this unless set explicitly |
| `RB_VNC_FPS` | `4` | frames a second; the cost table is in its comment |
| `RB_VNC_MODE` | `raw` | the mode to start in, if the mode file is absent |
| `RB_VNC_MODE_FILE` | `/run/rblive4/vnc.mode` | the switch's control file |
| `RB_VNC_ZLIB_LEVEL` | `1` | the deflate level for **both** compressing encodings, Tight (7) and zlib (6); `0` means "send everything Raw", not "compress at level 0" |
| `RB_VNC_INPUT` | `0` | pointer and keys back into rbp — **not yet implemented** |

**`RB_VNC_ZLIB_LEVEL` reaches the process as `--zlib-level`, not through the
environment.** `rb_load_conf` sets shell variables, it does not export them, so a value
sitting in `rb.conf` and read by nobody would look exactly like a value that works —
the failure mode this repo keeps re-finding. `vnc-run.sh` therefore passes it as a flag,
and the environment variable is only the fallback for a server started by hand.

## What is not done yet

**Neither the service nor an `install.sh` deploy has ever run on a unit** (drill `S11.7`,
in [13-raspberrypi4.md](13-raspberrypi4.md)). What is being measured today is a
hand-started process out of `/tmp/vncprobe/`, and **`/tmp` is tmpfs** — a reboot loses it,
and it is not the code path `vnc-run.sh` describes. Everything below it in this document
was measured against that process, so the measurements stand; the *deployment* does not.

**Input is step 5 of the build order and has not been started.** Pointer and key
events are meant to be injected into the panel's own nodes exactly as `work/poke.py`
and `work/keysend.py` do, behind `RB_VNC_INPUT`. The one thing that must be measured
rather than assumed is the **evdev record size**: the kernel is aarch64 (64-bit
`struct input_event` = 24 B) while the userspace is 32-bit (16 B), and `poke.py`
writes 16 while `keysend.py` writes 24 — both cannot be right. The design is a startup
lone `SYN_REPORT` (a genuine no-op) written at 24 bytes and then at 16, using
whichever `write()` accepts; if neither is, input is disabled and the log says so.

**`test_vnc_zlib.c` cannot run on the unit**, because it includes `<zlib.h>` and the unit
has zlib's *runtime*, not its headers. The suite is Mac-only by design and
`make test` on the unit will stop at it; run the other five explicitly, or make the
target tolerantly on the host.

## See also

* [06-display.md](06-display.md) — the framebuffer, the present modes, and the plane
  gap this document's capture works around.
* [12-troubleshooting.md](12-troubleshooting.md) — the `## VNC` symptom → cause → fix
  table.
* [13-raspberrypi4.md](13-raspberrypi4.md) — `S11`, the VNC drills, in the same shape
  as the hot-swap, LED and display drills.
* [scripts/device/README.md](../scripts/device/README.md) — the on-unit build, the
  launcher, and `rb.local.conf`.

## The screen in a browser (noVNC, `:5903`)

`rblive4-webvnc.service` runs `websockify`, which serves **noVNC** and reframes WebSocket bytes to
the RFB port. Open **`http://<unit>:5903/`** — `vnc.html` is the client and `webvnc-run.sh` links it
as `index.html` at startup, because websockify serves a directory *listing* for `/` when there is no
index.html. Either URL works; the bare one is the one to type. The password is the one the viewer asks a
native client for, because the session (including the password) is negotiated **end to end** between
the browser and `vncserve`. The bridge only moves bytes.

**Three edits are made to the installed pages**, by `novnc_patch.py`, on every start:

| page | what | why |
|---|---|---|
| `vnc_lite.html` | `showDotCursor: true` | that page draws **no cursor at all** when the server has not drawn one, and rbp never does — so the operator's pointer was invisible and the picture looked dead |
| `vnc_lite.html` | the `Send CtrlAltDel` button, its CSS, its handler and its function | nothing on this unit answers Ctrl-Alt-Del; rbp is a DJ player, not a desktop, and the button covered the picture |
| `app/ui.js` | the *"Running without HTTPS is not recommended…"* status line | this unit serves plain HTTP on the LAN **by design**, so the warning is not a finding — it is the permanent state of a working install, printed in red on every page load |

It is a patcher rather than a vendored copy of the page, for the reason `tools/build-directfb`
gives for its own tree: a copy in this repo would look like ours, rot against an `apt upgrade`,
and lose whatever the package fixed. The edits are anchored on text that only changes if noVNC
changes, each file is backed up once as `<name>.rbpi4b-orig`, an edit whose anchor does not match
is refused and named in the journal **without writing anything**, and the patcher always exits 0 —
an unpatched page is a worse page, never a dead client. Each file gains a stamp so a re-run is one
quiet line per file, and so a replaced (upgraded) file is patched again rather than skipped.

**Two reasons it exists, and the second is the interesting one:**

* it needs no client software on the machine looking at the screen;
* it is the **first client this unit can use its hardware JPEG encoder with**. JPEG may only be sent
  to a client that *advertised* a quality level, and Apple's Screen Sharing never does — so every
  macOS session rides zlib and `/dev/video11` sits idle. noVNC's Tight does advertise one.

**It is the same door, not a new one.** With sharing OFF there is no RFB listener for the bridge to
reach, and a browser connection simply fails — the same gate a native client has. The packages come
from the distribution (`apt-get install novnc websockify`); `install.sh` installs the unit only when
they are present and removes it otherwise, because `Restart=always` on a missing program is a restart
loop rather than a service.

**Proving it works without a browser — and the CONTROL matters.** A probe that only ever sees the
bridge cannot tell a broken bridge from its own bad parsing: the first version of this check ate the
greeting in the same read as the handshake response and reported a timeout that looked exactly like a
broken bridge. So read the greeting straight off the RFB port as well.

```sh
python3 - <<'PY'
import base64, os, socket
for port in (5901, 5903):          # 5901 is the CONTROL: the greeting straight off RFB
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    if port == 5903:
        k = base64.b64encode(os.urandom(16)).decode()
        s.sendall(("GET /websockify HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
                   "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
                   "Sec-WebSocket-Version: 13\r\n\r\n" % k).encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += s.recv(4096)
        data = buf.split(b"\r\n\r\n", 1)[1]
        while len(data) < 2:
            data += s.recv(4096)
        print(port, data[2:2 + (data[1] & 0x7F)])
    else:
        print(port, s.recv(24))
PY
# both print b'RFB 003.008\n'   (5901 is RB_VNC_PORT on this unit; the default is 5900)
```
