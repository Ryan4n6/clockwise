# Canvas clockface (`cw-cf-0x07`) and the moon face

How the Canvas clockface pulls its content, what the document fields actually
mean at the firmware level, and the traps that have bitten this project.

## What Canvas is

`cw-cf-0x07` renders a clockface described by a JSON document fetched over the
network instead of compiled into the firmware. Two NVS prefs point it at a
document:

| Pref | Value on Ryan's panel | Meaning |
| --- | --- | --- |
| `canvasServer` | `moon-canvas.ryan-massfeller.workers.dev` | Host to GET from |
| `canvasFile` | `moon` | Requested path is `/<canvasFile>.json` |

The device polls outward, so nothing needs to reach in to it. `deserializeDefinition()`
uses plain HTTP on port 80 by default. Only hosts starting with `raw.` are
fetched over 443, because a TLS handshake needs a large contiguous heap block
that the 64x64 HUB75 DMA framebuffers do not leave free at 8-bit color depth.
That is why the moon face used to have to drop the panel to 4-bit (a posterized
"duotone" moon) just to fetch its document.

## Document schema, as the firmware actually reads it

```json
{
  "name": "Moon", "author": "LumenLink", "version": 1,
  "bgColor": 0, "delay": 1000, "refresh": 1800000, "etag": "6b9679a9",
  "setup": [
    { "type": "image", "image": "<base64 png>", "x": 8, "y": 8 },
    { "type": "text",  "content": "20.0d", "font": "picopixel",
      "x": 1, "y": 6, "fgColor": 65535, "bgColor": 0 },
    { "type": "text",  "content": "SEVERE THUNDERSTORM WARNING", "font": "picopixel",
      "x": 1, "y": 53, "fgColor": 63488, "bgColor": 0,
      "w": 62, "scroll": "loop", "scrollMs": 40 }
  ]
}
```

- **`name` / `author` / `version` are effectively required.** `deserializeDefinition()`
  logs them with `Serial.printf("%s", ...)`. ArduinoJson returns `nullptr` for a
  missing key and `printf("%s", nullptr)` does `strlen(0x0)` -> LoadProhibited
  panic and a reboot loop. The firmware now defaults them with `|`, but emit them.
- **`delay` is `uint16_t`.** Anything over 65535 wraps. It only drives sprite
  frame timing. It is *not* a refresh interval, despite reading like one. The moon
  worker sent `delay: 1800000` for months, which arrived as `30528` and did
  nothing at all.
- **`refresh` is `uint32_t`, milliseconds, and is the re-fetch cadence** (added
  for clockwise#11). Missing means 30 minutes; anything under 15000 is clamped up
  to 15000 so a typo cannot turn the panel into a tight loop against the origin.
  The floor was 60000 until clockwise#13 lowered it: a face cannot be shown for
  less time than the fetch interval, so the old value capped rotation
  granularity at one minute.
- **`etag` is an optional content fingerprint** (clockwise#13). When it matches
  the last document the panel rendered, the refetch is dropped on the floor and
  nothing is repainted. This matters because `clockfaceSetup()` opens with a
  full-screen `fillRect`, so without it a 15 second refresh would flash the panel
  four times a minute, all night, redrawing identical pixels. Omit it and the
  firmware reads that as "cannot know" and repaints every time, which is the old
  behaviour and is safe, just wasteful. Any stable hash of the rendered elements
  works; the moon worker uses FNV-1a over `JSON.stringify(setup)`.
- **`w` / `scroll` / `scrollMs` on a `text` element** (clockwise#13). `w` is the
  box width in pixels, and it is the only one of the three that does anything on
  its own: an element that declares `w` is clipped to it, so a string too long
  for its box truncates at the edge instead of painting over its neighbours.
  `scroll` is `"none"` (default), `"once"` (slide out, hold, then park showing
  the *start* of the string, which is the part that identifies it) or `"loop"`
  (ping-pong forever). `scrollMs` is milliseconds per pixel of travel, default
  40. Anything unrecognised in `scroll` means `none`: a typo in a document should
  produce a boring panel, never a moving one. Text that fits inside `w` never
  scrolls no matter what it asks for.
- **`setup` elements render verbatim.** Only elements of `"type": "datetime"` are
  passed through ezTime's formatter; `text` elements are not. Backslash-escaping
  letters in a `text` element paints literal `\W\A\X...` on the panel.
- Colors are RGB565.

Firmware limits: a PNG must decode to <=1024 bytes, image width <=64, and the
JSON document buffer is 6144 bytes (`static DynamicJsonDocument doc` in
`Clockface.cpp`). Serve with an explicit `content-length`; the ESP32 reads the
raw body after the headers and cannot decode chunked transfer-encoding.

## The refresh loop (clockwise#11)

Before this fix the clockface fetched its document exactly once, in `setup()`,
and `update()` re-rendered only `datetime` elements. Every other element was
painted once and never touched again. A server-generated canvas like the moon
face therefore froze at its boot-time values: on 2026-09-02 the panel was showing
a moon it had downloaded on 2026-08-30, 2.8 days stale, while the worker was
serving the correct current phase.

`Clockface::update()` now calls `refetchCanvas()` every `_refreshMs`:

1. Stamp `_lastFetchMillis` with the *attempt*, so a failing origin backs off a
   full interval instead of retrying every loop.
2. On an `etag` match, return immediately. Nothing is cleared and nothing is
   repainted. Sprites and scrollers survive deliberately: the re-parsed document
   is byte-identical, so every `_spriteReference` and every `elementIndex` still
   points at what it did before, and `CustomSprite` holds a `uint8_t` index
   rather than a pointer into `doc`.
3. Otherwise `sprites.clear()` and `scrollers.clear()`, because their indexes
   referred to the previous document. This used to happen unconditionally at the
   top of the function, before the fetch; clockwise#13 moved it into the two
   branches that actually invalidate anything.
4. On success, `clockfaceSetup()` repaints, rebuilds sprites and scrollers, and
   picks up a new `refresh` value if the server changed it.
5. On failure, clear both anyway and reset `_lastEtag` so the next good fetch is
   forced to repaint. A failed parse leaves the shared `doc` clobbered, and a
   survivor would hand `renderImage()` a `nullptr`. The last good frame stays on
   the panel. No blank screen, no reboot.

One wrinkle worth knowing: `_lastEtag` is not set by the boot path, only by
`refetchCanvas()`. So the first refetch after any boot always repaints once even
if nothing changed. Harmless, since the panel had just painted that same content
at boot anyway, but it is why you will see one `repainted` before the `no
repaint` run starts.

Millis arithmetic is unsigned subtraction, so it survives the ~49-day rollover.

### Reading the panel's debug beacons

`DbgUdp.h` fires each `DBG()` string as a UDP packet to `192.168.1.5:5005` (the
pi-hole). Listen with:

```bash
ssh pi@100.105.25.23 "python3 -c \"
import socket,time
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(('0.0.0.0',5005))
while 1:
    d,a=s.recvfrom(512); print(time.strftime('%H:%M:%S'),a[0],d.decode('utf8','replace'),flush=True)
\""
```

Relevant markers: `CF3` fetch entered, `CF6` deserialize result, `CF7`/`CF9` boot
fetch and setup done, `CF10` refetch started, `CF11` refetch result (now one of
`repainted`, `unchanged, no repaint`, or `FAILED`), `CF12` how many scrollers the
document produced. Counting `CF9` tells you how many times the device has
*booted*: if that climbs, something is panicking.

## Scrolling text (clockwise#13)

Geometry lives in `firmware/lib/cw-textscroll/TextScroll.h`, deliberately free of
any Arduino header so it is unit-tested on the host with `pio test -e native`.
The clockface owns all drawing; that header owns all arithmetic. Note that
`firmware/lib/cw-textscroll/` is **not** gitignored, unlike `lib/cw-cf-*`, so it
is committed directly with no `sync.sh` dance.

Drawing composes into a shared 64x32 one-bit `GFXcanvas1` and blits the result.
Two traps in the pinned Adafruit_GFX make this less obvious than it looks, and
both were found the hard way:

- **Text wrap defaults to on** (`Adafruit_GFX.cpp:117`) and is honoured in both
  `charBounds`, the measuring path, and `write`, the drawing path, against the
  *surface* width. Measure a 100px string on a 64px surface with wrap left on and
  it reports back as 64px wide and two lines tall, so nothing ever looks like it
  overflows, no scroller is ever built, and the whole feature silently does
  nothing. `textCanvas()` sets `setTextWrap(false)` once, on construction.
- **`drawBitmap` recomputes its row stride** as `(w + 7) / 8` from the width you
  hand it (`Adafruit_GFX.cpp:1012`), while `GFXcanvas1` allocated its rows at the
  canvas width (`Adafruit_GFX.cpp:2027`). Passing the box width instead of the
  canvas width reads every row at the wrong offset and renders as diagonal hash.
  The fix is to zero the canvas columns past the box and blit the full canvas
  width using the transparent overload, which in turn needs an explicit
  `fillRect` erase first because a transparent blit paints only set bits. This
  is invisible at any box width from 57 to 64, so test it with a narrow box or
  you will not see it.

Both the static and the scrolling path go through `drawTextBoxed()`, so an
element that declares `w` is clipped identically whether it moves or not. An
element with no `w` takes the original unclipped code path untouched, which is
why every pre-existing document, the moon face included, renders as before.

## Server side (`moon-canvas` worker)

Lives in `~/Projects/moon-canvas`, deployed to Cloudflare as
`moon-canvas.ryan-massfeller.workers.dev`. It computes the lunar phase, picks one
of 100 photographic 48x48 frames, and returns the canvas document.

`cache-control` is `max-age=300`, deliberately well inside the panel's 30-minute
`refresh`. At the old `max-age=1800` a re-fetch could land on a nearly-expired
edge object and show a moon up to an hour old.

Its phase math is a mean-synodic model (constant 29.530588853-day cycle from a
known new moon). That matches a real ephemeris closely near the epoch of any
given cycle but ignores orbital eccentricity: peak illumination error is about
9.5 percentage points. Fine for a wall clock, wrong for anything that cares.

### `/hostile.json`, the firmware test face

The worker also serves a second route carrying deliberately hostile content: a
long line, a **narrow 24px box**, a string that fits, an empty string, a curly
apostrophe outside picopixel's `0x20..0x7E` range, and a looping line. It exists
so firmware changes can be verified against content designed to break them.

Point the panel at it by flipping one pref, and point it back the same way. The
moon face is never touched, so restoring does not need a redeploy:

```bash
curl -X POST "http://192.168.1.245/set?canvasFile=hostile"
curl -X POST "http://192.168.1.245/restart" && curl -s -o /dev/null http://192.168.1.245/get
# ... verify ...
curl -X POST "http://192.168.1.245/set?canvasFile=moon"
curl -X POST "http://192.168.1.245/restart" && curl -s -o /dev/null http://192.168.1.245/get
```

**A pref change does nothing until the device actually reboots.** The running
clockface keeps fetching the old `canvasFile` while `GET /get` cheerfully reports
the new one, so the two disagree and the panel is the one telling the truth. And
`POST /restart` returns 204 whether or not it reboots (clockwise#14), so always
confirm the reboot rather than assuming it: watch for a fresh `CF9` in the debug
beacon, or for the refetch cadence to change. Trailing the restart with a second
request, as above, is the current workaround.

## Where the forked source lives

`firmware/clockfaces/*` are upstream submodules (jnthas), so our edits cannot be
committed there. `firmware/lib/cw-cf-*` is gitignored, because that is where
PlatformIO's LDF wants exactly one clockface symlinked or copied at build time.

Our modified Canvas source is therefore tracked in
**`firmware/clockfaces-local/cw-cf-0x07/`**, and `firmware/clockfaces-local/sync.sh`
moves it:

```bash
./firmware/clockfaces-local/sync.sh check   # fail if snapshot and lib/ differ
./firmware/clockfaces-local/sync.sh push    # snapshot -> lib/ (after a clone/pull)
./firmware/clockfaces-local/sync.sh pull    # lib/ -> snapshot (before committing)
```

Run `check` before you trust a build, and `pull` before you commit. These two
copies had already silently drifted apart once.

## Flashing

```bash
cd firmware
FW_NAME=MOONRFSH PLATFORMIO_UPLOAD_PORT=192.168.1.245 \
  ~/.platformio-venv/bin/pio run -e ota -t upload
```

`upload_port` in `platformio.ini` is `clockwise.local`; override it with the IP
when mDNS does not cross your subnet. `FW_NAME` is required (it feeds
`-D CW_FW_NAME`) and is how you confirm afterwards which build is running:

```bash
ssh pi@100.105.25.23 "curl -s -D - -o /dev/null http://192.168.1.245/get" | grep -i 'CW_FW\|canvas'
```
