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
  "bgColor": 0, "delay": 1000, "refresh": 1800000,
  "setup": [
    { "type": "image", "image": "<base64 png>", "x": 8, "y": 8 },
    { "type": "text",  "content": "20.0d", "font": "picopixel",
      "x": 1, "y": 6, "fgColor": 65535, "bgColor": 0 }
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
  for clockwise#11). Missing means 30 minutes; anything under 60000 is clamped up
  to 60000 so a typo cannot turn the panel into a tight loop against the origin.
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
2. `sprites.clear()` **before** the fetch. `deserializeDefinition()` parses
   straight into the shared `doc`, so the moment it runs every sprite index
   points at the old document; a survivor would hand `renderImage()` a `nullptr`.
3. On success, `clockfaceSetup()` repaints and rebuilds sprites, and picks up a
   new `refresh` value if the server changed it.
4. On failure, the last good frame stays on the panel. No blank screen, no reboot.

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
fetch and setup done, `CF10` refetch started, `CF11` refetch result. Counting
`CF9` tells you how many times the device has *booted*: if that climbs, something
is panicking.

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
