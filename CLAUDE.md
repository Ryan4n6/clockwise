# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Fork of `jnthas/clockwise` — open-source ESP32-driven 64x64 HUB75 RGB LED matrix wall clock. The board exposes a web UI for configuration (WiFi, NTP/timezone, brightness, rotation, RGB order, clockface selection) and renders one of several pluggable "clockfaces" — each a separate Git repo pulled in as a submodule.

Remotes:
- `origin` → `Ryan4n6/clockwise` (this fork — push here)
- `upstream` → `jnthas/clockwise` (pull updates from here)

**Always pass `-R Ryan4n6/clockwise` to `gh`.** With both remotes present, a bare
`gh issue comment 15` resolved to `jnthas/clockwise` and posted onto a stranger's
PR #15 (deleted immediately, 2026-09-07). `gh repo set-default Ryan4n6/clockwise`
has been run, but the flag is the thing that cannot be wrong.

**`main` is the trunk.** PR #12 merged the fork's accumulated work into it on 2026-09-06 (OTA, auto-brightness, moon Canvas over HTTP, canvas re-fetch, rotation phase 1), and `fix/auto-brightness-floor` and `feature/arduino-ota` were deleted both locally and on origin. Any earlier note calling `fix/auto-brightness-floor` the real trunk is obsolete. Branch from `main`. Our `main` now diverges from `upstream/main`, so pulling upstream is a merge rather than a fast-forward.

## Build systems — there are TWO, and they share source

The same `firmware/src/main.cpp` is built by either system. The choice of clockface is the main differentiator.

### PlatformIO (one clockface per build — used by CI/release)

Source of truth for shipped binaries. CI matrix-builds one firmware binary per clockface and publishes them to `gh-pages` for the web flasher at clockwise.page.

```
cd firmware
pio run                                    # build for esp32dev (default env)
pio run -t upload                          # flash over USB
pio run -e ota -t upload                   # flash over WiFi (requires prior USB flash)
pio run -t monitor                         # serial monitor at 115200
pio test -e native                         # host-side unity tests (test_native/)
pio test -e esp32dev                       # on-device tests (test_embedded/)
```

The selected clockface is found via PlatformIO's Library Dependency Finder by symlinking it into `firmware/lib/` (CI does `ln -s ../clockfaces/<cw-cf-XX> firmware/lib/<cw-cf-XX>`). Without that symlink, `#include <Clockface.h>` won't resolve and the build will fail. Pick exactly one clockface at a time for PlatformIO builds.

Firmware version is hardcoded in `firmware/platformio.ini` (`CW_FW_VERSION`) and must be bumped on release per `CHECKLIST.md`.

### ESP-IDF (all clockfaces compiled in — Kconfig-selected at build time)

Top-level `CMakeLists.txt` + `main/main.cpp` (which `#include`s `firmware/src/main.cpp`) is the ESP-IDF entry. CI uses ESP-IDF v4.4.4. `main/CMakeLists.txt` lists the clockfaces as private requirements; `main/Kconfig.projbuild` exposes a `menuconfig` choice for which one to render.

```
idf.py menuconfig                          # pick clockface + HUB75 swap option
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

`sdkconfig.defaults` enables `CONFIG_AUTOSTART_ARDUINO=y` — Arduino runs as an ESP-IDF component (`components/arduino` submodule), which is why Arduino headers work in `main.cpp`.

## OTA — wireless firmware updates (added in this fork)

`ArduinoOTA` listens on TCP/3232 and advertises `_arduino._tcp` via mDNS. After one initial USB flash with the OTA code on the device, subsequent flashes work without unmounting:

```
pio run -e ota -t upload                   # uploads to clockwise.local with the build-time password
```

Hostname and password are set as build flags (`-D OTA_HOSTNAME`, `-D OTA_PASSWORD`) — defaults: `clockwise` / `clockwise`. Override at build time, or move to a `ClockwiseParams` NVS pref later if user-configurability is needed. Logs use the `[OTA]` prefix.

`ArduinoOTA.handle()` runs inside `loop()`'s WiFi-connected block. Filesystem updates (`U_SPIFFS`) are not used by this project; the firmware doesn't ship a SPIFFS image.

## Submodules — required before building

Everything under `components/` and `firmware/clockfaces/` is a Git submodule (see `.gitmodules`). A fresh clone has empty directories there, and builds will silently fail or produce confusing missing-header errors.

```
git submodule update --init --recursive    # ESP-IDF needs all of them
git submodule update --init firmware/clockfaces/   # PlatformIO only needs the clockfaces (deps come from lib_deps)
```

`components/arduino` (arduino-esp32) is large — first init takes a while.

## Architecture

`firmware/src/main.cpp` is a thin Arduino sketch (`setup()` / `loop()`) that wires together:

- **`firmware/lib/cw-commons/`** — shared infrastructure (no graphics):
  - `ClockwiseParams` (`CWPreferences.h`): singleton wrapping ESP32 NVS `Preferences` under namespace `"clockwise"`. Every user-visible setting (WiFi, timezone, brightness, RGB swap, driver, I2C speed, E-pin, etc.) is loaded here at boot and read by everything else. Adding a new setting means: new field + `PREF_*` key + load/save calls + a row in `SettingsWebPage.h`.
  - `WiFiController`: WiFiManager-based AP-mode fallback + Improv-WiFi serial provisioning. AP starts when SSID is empty (commit `33013df`). Also calls `MDNS.begin("clockwise")` after connect — ArduinoOTA reuses that mDNS instance.
  - `CWDateTime`: ezTime wrapper for NTP + POSIX timezone string + 12/24h.
  - `CWWebServer` / `SettingsWebPage`: HTTP server for `/settings` UI (port 80), runs only when WiFi is connected. Responds to `GET /` (HTML form), `GET /get` (current settings as `X-*` headers), `POST /set?key=value`, `POST /restart`. Hardware params (driver, I2C speed, E pin, RGB swap) are exposed here for tuning without reflashing.
  - `StatusController`: boot animation + LED blink patterns for status feedback.
  - `IClockface`: interface every clockface implements (`setup(CWDateTime*)`, `update()`).
- **`firmware/lib/cw-gfx-engine/`** — game-engine-style graphics primitives (`Sprite`, `Tile`, `Game`, `Object`, `EventBus`, `Locator`) used by clockfaces that animate (Mario, Pacman, Castlevania).
- **`firmware/clockfaces/cw-cf-0x0N/`** — each clockface is its own GitHub repo, must export a class named `Clockface` implementing `IClockface`. Canvas (`cw-cf-0x07`) is special: it renders a JSON-described theme fetched from a server, configured via `canvasFile` / `canvasServer` prefs. It re-fetches on the document's own `refresh` interval (default 30 min, floored at 15s); see `docs/canvas-clockface.md` for the document schema, the `delay`-is-uint16 trap, the `etag` repaint skip, the `w` / `scroll` / `scrollMs` text fields, and the UDP debug beacons.
- **`firmware/lib/cw-textscroll/`**: pure scroll geometry, no Arduino dependency, host-tested with `pio test -e native`. Unlike `firmware/lib/cw-cf-*` this is NOT gitignored, so commit it directly with no `sync.sh` step. Host test suites live in `test/test_native*`, and PlatformIO builds each suite directory into one binary, so a new suite needs its own directory rather than another file alongside `SimpleTests.cpp`.

## Forked clockface source lives in `clockfaces-local/`

`firmware/clockfaces/*` are upstream submodules, so our edits cannot be committed there, and `firmware/lib/cw-cf-*` is gitignored (it is the LDF target). Our modified sources are tracked in `firmware/clockfaces-local/<clockface>/`, with `firmware/clockfaces-local/sync.sh {check|push|pull}` moving them to and from `lib/`.

Run `sync.sh check` before trusting a build and `sync.sh pull` before committing. These copies drifted apart silently once already (clockwise#11).

Display I/O goes through `MatrixPanel_I2S_DMA` from the HUB75 component. `displaySetup()` in `main.cpp` is where GPIO remapping for RGB-order quirks (`swapBlueGreen`, `swapBlueRed`) and the configurable `driver` / `i2cSpeed` / `E_pin` get applied — these are the knobs to touch when a panel renders wrong colors or won't latch.

Auto-brightness uses an LDR on `ldrPin` (analog read), mapped through `autoBrightMin`/`autoBrightMax` into 10 slots → `setBrightness8`. Slot-change hysteresis (≥2 slots) avoids flicker.

## Adding a new clockface

Pattern from existing ones (and the CI workflow):

1. Create a separate repo `cw-cf-0xNN` exporting a `Clockface` class implementing `IClockface`, with its own `CMakeLists.txt` registering itself as an IDF component (see `cw-cf-0x07`'s as the canonical template per `CHECKLIST.md`).
2. Add it as a submodule under `firmware/clockfaces/`.
3. Append it to the matrix in `.github/workflows/clockwise-ci.yml` and to `CLOCKFACES` in `main/CMakeLists.txt`.
4. Add a `gh-pages` folder under `static/firmware/cw-cf-0xNN/` with a `manifest.json` for the web flasher.

## Release process

See `CHECKLIST.md` — manual today. Cutting a `releases/1.x.x` branch triggers `clockwise-ci.yml` which builds all clockfaces and pushes binaries to `gh-pages`. The version string in `firmware/platformio.ini` must be bumped first.

## Known device

Ryan's clock is at `192.168.1.245`. Settings UI: http://192.168.1.245/. Running `1.4.2` / `CW_GHOSTFIX` on `cw-cf-0x07` (Canvas), pointed at the panel worker. The worker rotates four faces (moon, weather, agenda, gameday) on five minute slots, with anything at urgency 5 or over holding the panel instead; the note here that weather rests permanently is obsolete as of moon-canvas#12, which added the time term the resolver never had. `CW_GHOSTFIX` carries clockwise#15 (scroller state survives a content change when the document's `face` is unchanged), clockwise#16 (renderText erases the PREVIOUS extent, so a narrowing string cannot leave a ghost) and clockwise#17 (`GET /measure`). The `192.168.1.44` address in earlier notes is stale.

The device answers `GET /measure?<font>=<text>` (clockwise#17), returning what
Adafruit_GFX measures that string as on the hardware: `X-w`, `X-h`, `X-x1`,
`X-y1`, plus `X-wrapW`/`X-wrapH` for the same string through the display, whose
text wrap is on. The font is the query KEY and the string is the value, `+` is a
space, and `%` is not decoded. `moon-canvas/scripts/pin-textbounds.mjs` walks a
corpus through it and writes `test-vectors/textbounds.tsv`; re-run it after any
font or text change on either side. Its first run found a real bug in the
worker's port of getTextBounds (moon-canvas#22).

**Set `FW_NAME` when you flash locally.** `platformio.ini` reads it from the
environment and a bare `pio run -t upload` leaves `X-CW_FW_NAME` empty, which
destroys the only marker that says which build is on the panel. It reports
`UNNAMED-LOCAL` rather than an empty string now, but a real name is the point:
`FW_NAME=CW_SOMETHING pio run -e ota -t upload`.

**A pref change does not take effect until the device actually reboots**, and `POST /restart` returns 204 whether or not it reboots (issue #14). The running clockface keeps using the old value while `GET /get` reports the new one, so the two disagree and the panel is what tells the truth. Always trail a restart with a second request and confirm the reboot (a fresh `CF9` in the UDP beacon, or the refetch cadence changing) rather than assuming it. This cost 90 minutes on 2026-09-05 with the panel stuck on test content after the restore was believed done.

The moon worker also serves `/hostile.json`, a deliberately hostile test face (long line, 24px narrow box, empty string, non-ASCII, looping line) for verifying firmware text rendering. Switch to it with `canvasFile=hostile` and back with `canvasFile=moon`; the moon face is never disturbed, so restoring needs no redeploy.

The laptop is not always on that LAN. Reach the device through the pi-hole over Tailscale:

```bash
ssh pi@100.105.25.23 "curl -s -D - -o /dev/null http://192.168.1.245/get" | grep -i canvas
ssh pi@100.105.25.23 "curl -s -X POST http://192.168.1.245/restart"
```

`restore-matrix.sh` in `~/Projects/moon-canvas` puts the canvas prefs back to their pre-moon state.
