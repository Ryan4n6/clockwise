# Panel Rotation Phase 1 (firmware) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `cw-cf-0x07` the three firmware capabilities that multi-face rotation depends on: skip the repaint when content is unchanged, allow sub-minute refresh intervals, and scroll text that is too wide for its box.

**Architecture:** The scroll geometry is extracted into a standalone header-only library with zero Arduino dependencies, so it is unit-tested on the host via `pio test -e native`. The clockface consumes it and handles only the drawing. Text is composed into a `GFXcanvas1` sized to the element's box and blitted, which gives right-edge clipping for free rather than requiring manual clip math.

**Tech Stack:** C++11, PlatformIO, Adafruit_GFX (`GFXcanvas1`), ArduinoJson 6, Unity test framework, ESP32 Arduino core.

**Spec:** `../specs/2026-09-04-panel-rotation-design.md` (in the `moon-canvas` repo at `docs/superpowers/specs/2026-09-04-panel-rotation-design.md`)

## Global Constraints

- **No em dashes or en dashes** in any file, comment, commit message, or output. Use a comma, colon, parentheses, or two sentences.
- **Every non-trivial change files a GitHub issue first** and references it as `#13` in every commit.
- **`firmware/lib/cw-cf-*` is gitignored.** It is the PlatformIO LDF build copy. The tracked source of truth is `firmware/clockfaces-local/cw-cf-0x07/`. Run `./firmware/clockfaces-local/sync.sh pull` before every commit that touches the clockface, and `sync.sh check` before trusting a build.
- **`firmware/lib/cw-textscroll/` is NOT gitignored** (the pattern is `cw-cf-*` only). It is committed directly with no sync dance.
- **`FW_NAME` is a required build variable.** Builds run as `FW_NAME=<name> pio run`. It feeds `-D CW_FW_NAME` and is how the running build is identified over HTTP.
- **PlatformIO lives at `/Users/admin/.platformio-venv/bin/pio`**, not on `PATH`.
- **Canvas document limits, enforced by firmware and not negotiable:** JSON buffer 6144 bytes, PNG decodes to under 1024 bytes, image width at most 64 px, `delay` is `uint16_t`, `name`/`author`/`version` must be present.
- **Only `type: "datetime"` elements re-render between fetches.** Everything else is painted once per fetch.

---

### Task 1: Pure scroll geometry, host-tested

Extract the scroll math into a standalone header with no Arduino dependency so it can be tested on the host. This is the only part of scrolling with real logic in it; everything downstream is drawing.

**Files:**
- Create: `firmware/lib/cw-textscroll/TextScroll.h`
- Create: `firmware/lib/cw-textscroll/library.json`
- Test: `firmware/test/test_native/TextScrollTests.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `enum ScrollMode { SCROLL_NONE = 0, SCROLL_ONCE = 1, SCROLL_LOOP = 2 };` and `struct ScrollResult { int16_t offsetX; bool done; };` and `ScrollResult textScrollOffset(uint16_t textW, uint16_t boxW, uint32_t elapsedMs, uint16_t scrollMs, uint8_t mode);` Task 3 calls this.

**Semantics to implement:**

- `textW <= boxW` means the text fits: offset 0, `done` true, regardless of mode. Nothing that fits ever moves.
- `mode == SCROLL_NONE`: offset 0, `done` true.
- `scrollMs == 0` is treated as 1 to avoid divide by zero.
- `travel = textW - boxW`, the number of pixels the text must slide to reveal its tail.
- `step = elapsedMs / scrollMs`, the current pixel position.
- `SCROLL_ONCE`: slide out to `-travel`, hold for `SCROLL_HOLD_STEPS`, then return to offset 0 and report `done`. Parking at the start rather than the end is deliberate: a parked line should show the beginning of the string, which is the part that identifies it.
- `SCROLL_LOOP`: ping-pong forever, `0` to `-travel` to `0`, never `done`. Ping-pong rather than wrap-around because wrap requires drawing the string twice per frame for a seamless join, and ping-pong reuses the same arithmetic. It also matches the `shouldReturnToOrigin` precedent already in `handleSpriteMovement()`.

- [ ] **Step 1: Write the failing tests**

Create `firmware/test/test_native/TextScrollTests.cpp`:

```cpp
#include "unity.h"
#include "TextScroll.h"

void setUp(void) {}
void tearDown(void) {}

// Text that fits never moves, whatever the mode says.
void test_fitting_text_never_scrolls(void) {
  ScrollResult r = textScrollOffset(40, 62, 100000, 40, SCROLL_LOOP);
  TEST_ASSERT_EQUAL_INT16(0, r.offsetX);
  TEST_ASSERT_TRUE(r.done);
}

// Exactly box width is "fits", not "overflows by zero".
void test_exact_fit_never_scrolls(void) {
  ScrollResult r = textScrollOffset(62, 62, 100000, 40, SCROLL_ONCE);
  TEST_ASSERT_EQUAL_INT16(0, r.offsetX);
  TEST_ASSERT_TRUE(r.done);
}

void test_mode_none_never_scrolls(void) {
  ScrollResult r = textScrollOffset(200, 62, 100000, 40, SCROLL_NONE);
  TEST_ASSERT_EQUAL_INT16(0, r.offsetX);
  TEST_ASSERT_TRUE(r.done);
}

// One pixel per scrollMs.
void test_once_advances_one_px_per_interval(void) {
  TEST_ASSERT_EQUAL_INT16(0,  textScrollOffset(100, 62, 0,   40, SCROLL_ONCE).offsetX);
  TEST_ASSERT_EQUAL_INT16(-1, textScrollOffset(100, 62, 40,  40, SCROLL_ONCE).offsetX);
  TEST_ASSERT_EQUAL_INT16(-5, textScrollOffset(100, 62, 200, 40, SCROLL_ONCE).offsetX);
}

// travel = 100 - 62 = 38. Never slides past that.
void test_once_clamps_at_travel(void) {
  ScrollResult r = textScrollOffset(100, 62, 38 * 40, 40, SCROLL_ONCE);
  TEST_ASSERT_EQUAL_INT16(-38, r.offsetX);
  TEST_ASSERT_FALSE(r.done);
}

// Holds at the end before returning.
void test_once_holds_at_end(void) {
  ScrollResult r = textScrollOffset(100, 62, (38 + 5) * 40, 40, SCROLL_ONCE);
  TEST_ASSERT_EQUAL_INT16(-38, r.offsetX);
  TEST_ASSERT_FALSE(r.done);
}

// After the hold it parks at the START of the string and reports done.
void test_once_parks_at_start_and_finishes(void) {
  ScrollResult r = textScrollOffset(100, 62, (38 + SCROLL_HOLD_STEPS) * 40, 40, SCROLL_ONCE);
  TEST_ASSERT_EQUAL_INT16(0, r.offsetX);
  TEST_ASSERT_TRUE(r.done);
}

// Once done, it stays done however long you wait.
void test_once_stays_done(void) {
  ScrollResult r = textScrollOffset(100, 62, 9999999, 40, SCROLL_ONCE);
  TEST_ASSERT_EQUAL_INT16(0, r.offsetX);
  TEST_ASSERT_TRUE(r.done);
}

// Ping-pong: out to -travel, then back to 0, and never done.
void test_loop_pingpongs(void) {
  TEST_ASSERT_EQUAL_INT16(-38, textScrollOffset(100, 62, 38 * 40, 40, SCROLL_LOOP).offsetX);
  TEST_ASSERT_EQUAL_INT16(-20, textScrollOffset(100, 62, 56 * 40, 40, SCROLL_LOOP).offsetX);
  TEST_ASSERT_EQUAL_INT16(0,   textScrollOffset(100, 62, 76 * 40, 40, SCROIL_LOOP_TYPO_GUARD).offsetX);
  TEST_ASSERT_FALSE(textScrollOffset(100, 62, 9999999, 40, SCROLL_LOOP).done);
}

// Degenerate inputs must not divide by zero or wrap.
void test_zero_scrollms_is_treated_as_one(void) {
  ScrollResult r = textScrollOffset(100, 62, 10, 0, SCROLL_ONCE);
  TEST_ASSERT_EQUAL_INT16(-10, r.offsetX);
}

void test_zero_box_width_does_not_crash(void) {
  ScrollResult r = textScrollOffset(100, 0, 400, 40, SCROLL_ONCE);
  TEST_ASSERT_EQUAL_INT16(-10, r.offsetX);
}

void test_zero_text_width_fits(void) {
  ScrollResult r = textScrollOffset(0, 62, 400, 40, SCROLL_ONCE);
  TEST_ASSERT_EQUAL_INT16(0, r.offsetX);
  TEST_ASSERT_TRUE(r.done);
}

int runUnityTests(void) {
  UNITY_BEGIN();
  RUN_TEST(test_fitting_text_never_scrolls);
  RUN_TEST(test_exact_fit_never_scrolls);
  RUN_TEST(test_mode_none_never_scrolls);
  RUN_TEST(test_once_advances_one_px_per_interval);
  RUN_TEST(test_once_clamps_at_travel);
  RUN_TEST(test_once_holds_at_end);
  RUN_TEST(test_once_parks_at_start_and_finishes);
  RUN_TEST(test_once_stays_done);
  RUN_TEST(test_loop_pingpongs);
  RUN_TEST(test_zero_scrollms_is_treated_as_one);
  RUN_TEST(test_zero_box_width_does_not_crash);
  RUN_TEST(test_zero_text_width_fits);
  return UNITY_END();
}

int main() { runUnityTests(); }
```

**Note:** `SCROIL_LOOP_TYPO_GUARD` in `test_loop_pingpongs` is intentional and must be corrected to `SCROLL_LOOP` when you type this in. It exists so that a worker who pastes this file without reading it gets a compile error rather than a silently wrong test.

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cd /Users/admin/Projects/clockwise/firmware
/Users/admin/.platformio-venv/bin/pio test -e native
```

Expected: FAIL, `TextScroll.h: No such file or directory`.

- [ ] **Step 3: Write the library manifest**

Create `firmware/lib/cw-textscroll/library.json`:

```json
{
  "name": "cw-textscroll",
  "version": "1.0.0",
  "description": "Pure scroll geometry for canvas text elements. No Arduino dependency so it unit-tests on the host.",
  "frameworks": "*",
  "platforms": "*"
}
```

- [ ] **Step 4: Write the implementation**

Create `firmware/lib/cw-textscroll/TextScroll.h`:

```cpp
#pragma once
#include <stdint.h>

// Scroll geometry for canvas text elements, kept free of any Arduino header so
// it can be unit-tested on the host with `pio test -e native`. The clockface
// owns all drawing; this file owns all arithmetic.

enum ScrollMode {
  SCROLL_NONE = 0,   // never move, truncate at the box edge
  SCROLL_ONCE = 1,   // run through once on arrival, then park at the start
  SCROLL_LOOP = 2,   // ping-pong forever
};

// How many pixel-steps to hold at the far end of a SCROLL_ONCE pass before
// snapping back. 25 steps at the default 40 ms/step is a one second pause,
// which is about how long it takes to read the tail of a line.
#define SCROLL_HOLD_STEPS 25

struct ScrollResult {
  int16_t offsetX;   // px to shift the text left by (0 or negative)
  bool    done;      // true when this scroller needs no further redraws
};

inline ScrollResult textScrollOffset(uint16_t textW,
                                     uint16_t boxW,
                                     uint32_t elapsedMs,
                                     uint16_t scrollMs,
                                     uint8_t  mode)
{
  ScrollResult r = { 0, true };

  // Anything that fits is static, whatever the document asked for. This is also
  // the guard that makes `travel` below safe to compute as an unsigned subtract.
  if (mode == SCROLL_NONE || textW <= boxW) return r;

  if (scrollMs == 0) scrollMs = 1;            // never divide by zero

  const uint32_t travel = (uint32_t)(textW - boxW);
  const uint32_t step   = elapsedMs / scrollMs;

  if (mode == SCROLL_LOOP) {
    // Ping-pong. Wrap-around would need the string drawn twice per frame for a
    // seamless join; ping-pong reuses this arithmetic and matches the
    // shouldReturnToOrigin behaviour already in handleSpriteMovement().
    const uint32_t cycle = travel * 2;
    const uint32_t p     = step % cycle;
    r.offsetX = (p <= travel) ? -(int16_t)p : -(int16_t)(cycle - p);
    r.done    = false;
    return r;
  }

  // SCROLL_ONCE
  if (step < travel) {                         // sliding out
    r.offsetX = -(int16_t)step;
    r.done    = false;
  } else if (step < travel + SCROLL_HOLD_STEPS) {
    r.offsetX = -(int16_t)travel;              // holding at the tail
    r.done    = false;
  } else {
    r.offsetX = 0;                             // parked at the head, finished
    r.done    = true;
  }
  return r;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cd /Users/admin/Projects/clockwise/firmware
/Users/admin/.platformio-venv/bin/pio test -e native
```

Expected: PASS, 12 test cases (13 including the pre-existing `SimpleTests.cpp` case).

If `test_zero_box_width_does_not_crash` fails, the unsigned subtract guard is wrong: `boxW == 0` with `textW == 100` must give `travel == 100`, and `400/40 == 10` steps, so offset `-10`.

- [ ] **Step 6: Commit**

```bash
cd /Users/admin/Projects/clockwise
git add firmware/lib/cw-textscroll firmware/test/test_native/TextScrollTests.cpp
git commit -m "feat(textscroll): pure scroll geometry with host tests (#13)"
```

---

### Task 2: `etag` repaint skip and the 15 s refresh floor

Two small independent changes to the same two files, landed together because they share a build and a flash.

**Files:**
- Modify: `firmware/lib/cw-cf-0x07/Clockface.h:31` (the `CANVAS_MIN_REFRESH_MS` define) and the private member block near `:58`
- Modify: `firmware/lib/cw-cf-0x07/Clockface.cpp:65` (`refetchCanvas()`)

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: a `String _lastEtag` member. Task 3 does not touch it.

**Why the sprite clear moves.** `refetchCanvas()` currently calls `sprites.clear()` before `deserializeDefinition()`, because parsing invalidates every sprite index. On an `etag` match the newly parsed document has byte-identical content, so those indexes are still correct. `CustomSprite` holds only a `uint8_t _spriteReference` and no pointer into the document (verified in `CustomSprite.h`), so surviving sprites are safe. The clear therefore moves into the repaint branch and the failure branch, and is skipped on a match.

- [ ] **Step 1: Lower the refresh floor**

In `firmware/lib/cw-cf-0x07/Clockface.h`, replace:

```cpp
#define CANVAS_MIN_REFRESH_MS 60000UL
```

with:

```cpp
// Floor the requested rate so a bad/zero "refresh" can't turn the panel into a
// tight HTTP loop against the origin. Lowered from 60000 for multi-face
// rotation: a face cannot be shown for less time than the fetch interval, so
// the old floor capped rotation granularity at one minute.
#define CANVAS_MIN_REFRESH_MS 15000UL
```

- [ ] **Step 2: Add the etag member**

In `firmware/lib/cw-cf-0x07/Clockface.h`, immediately after the `uint32_t _refreshMs = CANVAS_DEFAULT_REFRESH_MS;` line, add:

```cpp
  // Last rendered document's "etag". With refresh capped at 5 minutes for
  // rotation, the panel re-fetches 288 times a day, and clockfaceSetup() opens
  // with a full-screen fillRect. Repainting an unchanged document would flash
  // the panel every 5 minutes all night. Empty means "no etag seen yet".
  String _lastEtag;
```

- [ ] **Step 3: Restructure `refetchCanvas()`**

In `firmware/lib/cw-cf-0x07/Clockface.cpp`, replace the whole body of `refetchCanvas()` with:

```cpp
void Clockface::refetchCanvas()
{
  // Stamp the attempt, not the success: a failing origin must not put us into a
  // retry-every-loop hammer against it. Next try is one full interval out.
  _lastFetchMillis = millis();
  DBG("CF10 canvas refetch start");

  if (deserializeDefinition())
  {
    // An unchanged document means an unchanged panel. Skip the repaint entirely
    // rather than fillRect the screen and redraw identical pixels (#13).
    // Sprites survive: the re-parsed content is byte-identical, so every
    // _spriteReference index still points at what it did before, and
    // CustomSprite holds an index rather than a pointer into `doc`.
    const char *etag = doc["etag"] | "";
    if (etag[0] != '\0' && _lastEtag == etag)
    {
      DBG("CF11 canvas refetch OK, unchanged, no repaint");
      return;
    }
    _lastEtag = etag;

    // Content changed (or the document carries no etag, so we cannot know).
    // Drop sprites before rebuilding: their indexes referred to the previous
    // document and clockfaceSetup() recreates them from the new one.
    sprites.clear();
    clockfaceSetup();
    DBG("CF11 canvas refetch OK, repainted");
  }
  else
  {
    // deserializeDefinition() already drew its own error splash. A failed parse
    // leaves the shared `doc` clobbered, so any surviving sprite would index
    // into it and hand renderImage() a nullptr -> LoadProhibited panic.
    sprites.clear();
    _lastEtag = "";   // force a repaint on the next good fetch
    DBG("CF11 canvas refetch FAILED, keeping last frame");
  }
}
```

- [ ] **Step 4: Build**

```bash
cd /Users/admin/Projects/clockwise/firmware
FW_NAME=MOONETAG /Users/admin/.platformio-venv/bin/pio run -e esp32dev
```

Expected: SUCCESS. If `String` is undeclared, `Clockface.h` already includes `<Arduino.h>` at the top; confirm that include survived.

- [ ] **Step 5: Sync the tracked snapshot and commit**

```bash
cd /Users/admin/Projects/clockwise
./firmware/clockfaces-local/sync.sh pull
./firmware/clockfaces-local/sync.sh check
git add firmware/clockfaces-local
git commit -m "feat(cw-cf-0x07): skip repaint on unchanged etag, drop refresh floor to 15s (#13)"
```

---

### Task 3: The text scroll primitive

**Files:**
- Modify: `firmware/lib/cw-cf-0x07/Clockface.h` (include, scroller struct, member vector, method declarations)
- Modify: `firmware/lib/cw-cf-0x07/Clockface.cpp:117` (`renderText`), `:154` (`clockfaceSetup`), `:296` (`clockfaceLoop`)

**Interfaces:**
- Consumes: `textScrollOffset()`, `ScrollMode`, `ScrollResult`, `SCROLL_HOLD_STEPS` from Task 1.
- Produces: nothing consumed by later tasks in this plan. Phase 2 faces emit the `w` / `scroll` / `scrollMs` fields this reads.

**Why `GFXcanvas1` rather than clipping by hand.** Drawing text at a negative `x` clips at the screen's left edge for free, because `writePixel` bounds-checks. It does nothing about the right edge: a 100 px string in a 62 px box would overdraw whatever sits beside it. `GFXcanvas1` is an `Adafruit_GFX` subclass, so `setFont` / `setCursor` / `print` work on it unchanged. Compose into a canvas the size of the box, then `drawBitmap` it at the box origin, and both edges clip correctly with no manual math. A 64x16 1-bit canvas is 128 bytes, so one shared static instance covers every scroller.

- [ ] **Step 1: Write the failing test**

The drawing cannot be tested on the host (it needs a panel), but the element parsing can be. Add to `firmware/test/test_native/TextScrollTests.cpp`, before `runUnityTests`:

```cpp
// Mirrors Clockface::scrollModeFromName. Kept in the pure library so the
// mapping from document string to enum is tested without a panel.
void test_mode_name_parsing(void) {
  TEST_ASSERT_EQUAL_UINT8(SCROLL_NONE, scrollModeFromName(nullptr));
  TEST_ASSERT_EQUAL_UINT8(SCROLL_NONE, scrollModeFromName(""));
  TEST_ASSERT_EQUAL_UINT8(SCROLL_NONE, scrollModeFromName("none"));
  TEST_ASSERT_EQUAL_UINT8(SCROLL_ONCE, scrollModeFromName("once"));
  TEST_ASSERT_EQUAL_UINT8(SCROLL_LOOP, scrollModeFromName("loop"));
  TEST_ASSERT_EQUAL_UINT8(SCROLL_NONE, scrollModeFromName("nonsense"));
}
```

and register it with `RUN_TEST(test_mode_name_parsing);`.

- [ ] **Step 2: Run the tests to verify the new one fails**

```bash
cd /Users/admin/Projects/clockwise/firmware
/Users/admin/.platformio-venv/bin/pio test -e native
```

Expected: FAIL, `scrollModeFromName` was not declared in this scope.

- [ ] **Step 3: Add the parser to the pure library**

Append to `firmware/lib/cw-textscroll/TextScroll.h`, before the final newline:

```cpp
#include <string.h>

// Maps a document's "scroll" string to a mode. Anything unrecognised, missing,
// or null means "do not move", because a typo in a canvas document should
// produce a boring panel and never a moving one.
inline uint8_t scrollModeFromName(const char *name)
{
  if (name == nullptr)          return SCROLL_NONE;
  if (strcmp(name, "once") == 0) return SCROLL_ONCE;
  if (strcmp(name, "loop") == 0) return SCROLL_LOOP;
  return SCROLL_NONE;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cd /Users/admin/Projects/clockwise/firmware
/Users/admin/.platformio-venv/bin/pio test -e native
```

Expected: PASS, 13 cases from `TextScrollTests.cpp`.

- [ ] **Step 5: Commit the pure part**

```bash
cd /Users/admin/Projects/clockwise
git add firmware/lib/cw-textscroll firmware/test/test_native/TextScrollTests.cpp
git commit -m "feat(textscroll): parse scroll mode names, unknown means static (#13)"
```

- [ ] **Step 6: Make font selection work on any surface**

`Clockface::setFont()` hardcodes `Locator::getDisplay()`. The scroller needs the
same font applied to an off-screen canvas, and the pinned Adafruit_GFX has no
`getFont()` to read it back with (verified against the installed header). Split
the chain so it takes a target.

In `firmware/lib/cw-cf-0x07/Clockface.h`, in the private method block, add:

```cpp
  void setFontOn(Adafruit_GFX *target, const char *fontName);
```

In `firmware/lib/cw-cf-0x07/Clockface.cpp`, replace the whole of
`Clockface::setFont` with:

```cpp
// Font selection against an arbitrary surface. The panel and the off-screen
// scroll canvas both need it, and this Adafruit_GFX has no getFont() to read the
// current selection back with, so the caller names the font every time.
void Clockface::setFontOn(Adafruit_GFX *target, const char *fontName)
{
  if (fontName == nullptr)
  {
    target->setFont();
  }
  else if (strcmp(fontName, "picopixel") == 0)
  {
    target->setFont(&Picopixel);
  }
  else if (strcmp(fontName, "square") == 0)
  {
    target->setFont(&atariFont);
  }
  else if (strcmp(fontName, "big") == 0)
  {
    target->setFont(&hour8pt7b);
  }
  else if (strcmp(fontName, "medium") == 0)
  {
    target->setFont(&minute7pt7b);
  }
  else
  {
    target->setFont();
  }
}

void Clockface::setFont(const char *fontName)
{
  setFontOn(Locator::getDisplay(), fontName);
}
```

Note the added `nullptr` guard: the original passed `fontName` straight to
`strcmp`, so a text element with no `font` key would have dereferenced null.

- [ ] **Step 7: Declare the scroller in the header**

In `firmware/lib/cw-cf-0x07/Clockface.h`, add to the include block near the other project includes:

```cpp
#include <TextScroll.h>
```

Add above `class Clockface`:

```cpp
// One overflowing text element being animated. Position and geometry are copied
// out of the document at setup time rather than re-read every frame, so a frame
// costs no JSON traversal.
struct TextScroller {
  uint8_t  elementIndex;   // index into doc["setup"]
  int16_t  x, y;           // element origin on the panel
  uint16_t boxW;           // clip width; text wider than this scrolls
  uint16_t textW, textH;   // measured extent of the full string
  int16_t  boundsX, boundsY; // getTextBounds offsets, needed to erase correctly
  uint16_t scrollMs;
  uint8_t  mode;
  uint32_t startMs;
  bool     done;
  int16_t  lastOffsetX;    // skip the redraw when the offset has not changed
};
```

Add to the private member block, next to `sprites`:

```cpp
  std::vector<TextScroller> scrollers;

  void buildScrollers();
  void scrollLoop();
  void drawScroller(TextScroller &s, int16_t offsetX);
```

- [ ] **Step 8: Build the scroller list at setup**

In `firmware/lib/cw-cf-0x07/Clockface.cpp`, add this function immediately before `void Clockface::clockfaceLoop()`:

```cpp
void Clockface::buildScrollers()
{
  scrollers.clear();

  JsonArrayConst elements = doc["setup"].as<JsonArrayConst>();
  uint8_t idx = 0;
  for (JsonVariantConst value : elements)
  {
    const char *type = value["type"].as<const char *>();
    if (type == nullptr || strcmp(type, "text") != 0) { idx++; continue; }

    const uint8_t mode = scrollModeFromName(value["scroll"].as<const char *>());
    if (mode == SCROLL_NONE) { idx++; continue; }

    const char *content = value["content"].as<const char *>();
    if (content == nullptr) { idx++; continue; }

    TextScroller s;
    s.elementIndex = idx;
    s.x    = value["x"].as<int16_t>();
    s.y    = value["y"].as<int16_t>();
    // Default the box to "the rest of the panel", which is what a face means
    // when it does not say. Guard against x beyond the panel.
    s.boxW = value["w"] | (uint16_t)((s.x < 64) ? (64 - s.x) : 0);
    s.scrollMs = value["scrollMs"] | (uint16_t)40;
    s.mode = mode;

    // Measure with the element's own font, exactly as renderText() will.
    setFont(value["font"].as<const char *>());
    int16_t bx, by; uint16_t bw, bh;
    Locator::getDisplay()->getTextBounds(String(content), 0, 0, &bx, &by, &bw, &bh);
    s.textW = bw; s.textH = bh; s.boundsX = bx; s.boundsY = by;

    // Text that fits is not a scroller at all. renderElements() already drew it
    // statically and it must not be re-drawn every frame.
    if (s.textW <= s.boxW) { idx++; continue; }

    s.startMs     = millis();
    s.done        = false;
    s.lastOffsetX = 1;      // impossible offset, forces the first draw
    scrollers.push_back(s);
    idx++;
  }

  if (!scrollers.empty()) {
    DBG((String("CF12 scrollers=") + String((int)scrollers.size())).c_str());
  }
}
```

- [ ] **Step 9: Draw one scroller frame**

Add immediately after `buildScrollers()`:

```cpp
void Clockface::drawScroller(TextScroller &s, int16_t offsetX)
{
  JsonVariantConst value = doc["setup"][s.elementIndex];
  const char *content = value["content"].as<const char *>();
  if (content == nullptr) return;

  const uint16_t fg = value["fgColor"].as<const uint16_t>();
  const uint16_t bg = value["bgColor"].as<const uint16_t>();

  // Compose into a 1-bit canvas the width of the box. Drawing at a negative x
  // clips on the left for free (writePixel bounds-checks), but nothing clips the
  // right edge, and an overflowing string would paint over its neighbours. A
  // canvas sized to the box clips both edges with no manual math.
  // 64x16 at 1bpp is 128 bytes, so one shared instance serves every scroller.
  static GFXcanvas1 canvas(64, 16);

  const uint16_t w = (s.boxW > 64) ? 64 : s.boxW;
  const uint16_t h = (s.textH > 16) ? 16 : s.textH;
  if (w == 0 || h == 0) return;

  canvas.fillScreen(0);
  setFontOn(&canvas, value["font"].as<const char *>());
  canvas.setTextColor(1);
  // getTextBounds returns offsets relative to the cursor; subtract them so the
  // glyphs land inside the canvas rather than above its top edge.
  canvas.setCursor(offsetX - s.boundsX, -s.boundsY);
  canvas.print(content);

  Locator::getDisplay()->drawBitmap(s.x + s.boundsX, s.y + s.boundsY,
                                    canvas.getBuffer(), w, h, fg, bg);
}
```

- [ ] **Step 10: Run the scroll pass every loop**

In `firmware/lib/cw-cf-0x07/Clockface.cpp`, replace `clockfaceLoop()` with:

```cpp
void Clockface::clockfaceLoop() {
    // Sprites and scrollers are independent. The old early return here was on
    // `sprites.empty()`, which is true for every face in the rotation design, so
    // scrolling must not hang off it.
    for (auto& sprite : sprites) {
        handleSpriteAnimation(sprite);
    }
    scrollLoop();
}

void Clockface::scrollLoop() {
    if (scrollers.empty()) return;

    const uint32_t now = millis();
    for (auto& s : scrollers) {
        if (s.done) continue;

        ScrollResult r = textScrollOffset(s.textW, s.boxW,
                                          now - s.startMs, s.scrollMs, s.mode);

        // Redraw only when the pixel offset actually moved. At 40 ms/step the
        // main loop runs many times per step, and a redraw per loop would burn
        // the panel's frame budget repainting identical pixels.
        if (r.offsetX != s.lastOffsetX) {
            drawScroller(s, r.offsetX);
            s.lastOffsetX = r.offsetX;
        }
        s.done = r.done;
    }
}
```

- [ ] **Step 11: Wire setup and teardown**

In `clockfaceSetup()`, add `buildScrollers();` as the final statement, after `createSprites();`.

In `refetchCanvas()`, add `scrollers.clear();` on the line immediately after each existing `sprites.clear();` (there are two: the repaint branch and the failure branch). Scrollers hold `elementIndex` values into the previous document for exactly the same reason sprites do.

- [ ] **Step 12: Build**

```bash
cd /Users/admin/Projects/clockwise/firmware
FW_NAME=MOONSCRL /Users/admin/.platformio-venv/bin/pio run -e esp32dev
```

Expected: SUCCESS. `GFXcanvas1` is confirmed present in the pinned Adafruit_GFX (`Adafruit_GFX.h:319`). If the link fails on `Picopixel` or `atariFont` inside `setFontOn`, the font headers are already included by `Clockface.h` and the cause is include order, not a missing dependency.

- [ ] **Step 13: Sync and commit**

```bash
cd /Users/admin/Projects/clockwise
./firmware/clockfaces-local/sync.sh pull
./firmware/clockfaces-local/sync.sh check
git add firmware/clockfaces-local
git commit -m "feat(cw-cf-0x07): scroll text that overflows its box (#13)"
```

---

### Task 4: Hardware verification with hostile content

Nothing above proves anything on a panel. This task does, and it is not optional: every prior firmware change in this project was signed off by watching the real device.

**Files:**
- Create: `/private/tmp/.../scratchpad/hostile-canvas.js` (throwaway, not committed)
- Modify: nothing in the repo.

**Interfaces:**
- Consumes: the firmware from Tasks 2 and 3.
- Produces: evidence for the issue comment.

- [ ] **Step 1: Start the debug listener**

```bash
ssh pi@100.105.25.23 "nohup timeout 3600 python3 -c \"
import socket,time
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(('0.0.0.0',5005))
f=open('/tmp/moondbg.log','w',buffering=1)
while True:
    d,a=s.recvfrom(512)
    f.write(time.strftime('%H:%M:%S')+' '+d.decode('utf8','replace')+chr(10))
\" >/dev/null 2>&1 & sleep 1; echo listener-up"
```

- [ ] **Step 2: Flash over OTA**

```bash
cd /Users/admin/Projects/clockwise/firmware
FW_NAME=MOONSCRL PLATFORMIO_UPLOAD_PORT=192.168.1.245 \
  /Users/admin/.platformio-venv/bin/pio run -e ota -t upload
```

Expected: `Result attempt 1: 'OK'`, then SUCCESS.

- [ ] **Step 3: Confirm the new build is running**

```bash
ssh pi@100.105.25.23 "curl -s -m8 -D - -o /dev/null http://192.168.1.245/get" | grep -i 'CW_FW\|canvas'
```

Expected: `X-CW_FW_NAME: MOONSCRL`. A stale name means the flash did not take; do not proceed.

- [ ] **Step 4: Deploy hostile content**

In the `moon-canvas` repo, temporarily replace the `setup` array with these five text elements, keeping `name`/`author`/`version` and setting `refresh: 15000`:

| Case | Content | `w` | `scroll` |
| --- | --- | --- | --- |
| Long | `Sam - Evening Band Practice at the school` | 62 | `once` |
| Exactly box width | a string measured to exactly 62 px | 62 | `once` |
| Empty | `""` | 62 | `once` |
| Non-ASCII | `Jim Murphy` + U+2019 + `s Birthday` | 62 | `once` |
| Looping | `SEVERE THUNDERSTORM WARNING UNTIL 9PM` | 62 | `loop` |

Deploy with `npx wrangler deploy`, then verify with a cache-busted fetch before looking at the panel:

```bash
curl -s "http://moon-canvas.ryan-massfeller.workers.dev/moon.json?cb=$RANDOM" | python3 -m json.tool | head -30
```

- [ ] **Step 5: Watch the panel and the log**

```bash
ssh pi@100.105.25.23 "grep -E 'CF9|CF10|CF11|CF12' /tmp/moondbg.log | tail -20; echo '--- boots ---'; grep -c CF9 /tmp/moondbg.log"
```

Confirm all of the following. Any failure stops the task:

1. `CF12 scrollers=N` appears with the expected count. The empty string and the exactly-box-width string must NOT be counted as scrollers.
2. Long text scrolls once, then parks showing the beginning of the string.
3. The looping line ping-pongs and never stops.
4. The non-ASCII line does not crash the panel. Missing glyphs are acceptable at this stage; a reboot is not.
5. **Boot count (`CF9`) stays at 1.** This is the single most important check. Every past failure in this clockface showed up as a reboot loop.
6. Nothing overdraws its neighbours. This is what `GFXcanvas1` is there to prevent.

- [ ] **Step 6: Verify the etag skip**

With `refresh: 15000` and unchanged content, the panel fetches every 15 s. Confirm the log shows repeated `CF11 canvas refetch OK, unchanged, no repaint` and that the panel does **not** visibly flash.

Then change one character in the worker's content, redeploy, and confirm the next fetch logs `CF11 canvas refetch OK, repainted`.

- [ ] **Step 7: Restore the moon and clean up**

Revert the worker to the moon face, `npx wrangler deploy`, and confirm the panel returns to the moon with the correct current age and illumination. Then stop the listener and remove `/tmp/moondbg.log` from the pi.

- [ ] **Step 8: Close the issue**

Comment on the GitHub issue with: files changed, the six verification results above with the actual log lines, any divergences from this plan, follow-ups, and `[tokens: XXXk]`.

---

## Self-Review

**Spec coverage.** The spec's "Firmware changes required" section lists three items: the `etag` skip (Task 2), the 15 s floor (Task 2), and the text scroll primitive (Tasks 1 and 3). The canvas schema extension (`w`, `scroll`, `scrollMs`) is consumed in Task 3 Step 7. The spec's two named hazards, `clockfaceLoop()`'s empty-sprites early return and `refreshDateTime()` contention, are handled in Task 3 Step 9 and covered by the hardware check in Task 4 Step 5 item 6. The spec's hostile-content requirement is Task 4 Step 4. The scroll-box versus datetime-box overlap test belongs to the worker's conformance suite in Phase 2, not here, since no face in this phase emits both.

**Not covered here, by design.** Everything in the spec from "Faces" onward is Phase 2 and 3 worker work and needs its own plan.

**Type consistency.** `textScrollOffset(uint16_t, uint16_t, uint32_t, uint16_t, uint8_t) -> ScrollResult` is defined in Task 1 Step 4 and called in Task 3 Step 9 with `(s.textW, s.boxW, now - s.startMs, s.scrollMs, s.mode)`, all matching. `scrollModeFromName(const char*) -> uint8_t` is defined in Task 3 Step 3 and called in Task 3 Step 7. `TextScroller` fields set in Step 7 (`elementIndex`, `x`, `y`, `boxW`, `textW`, `textH`, `boundsX`, `boundsY`, `scrollMs`, `mode`, `startMs`, `done`, `lastOffsetX`) match the struct in Step 6 and the uses in Steps 8 and 9.

**Resolved risk.** An earlier draft had `drawScroller()` read the panel's current font back with `getFont()`. Checking the pinned Adafruit_GFX header showed no such method, so that line would not have compiled. Task 3 Step 6 now refactors `setFont` into `setFontOn(Adafruit_GFX*, const char*)` instead, which is the better shape anyway since the canvas and the panel are two surfaces needing the same font. `GFXcanvas1` was verified present at `Adafruit_GFX.h:319`.

**Bug found while writing the plan.** The existing `Clockface::setFont` passes `fontName` directly to `strcmp` with no null check, so a text element omitting `font` dereferences null. Task 3 Step 6 adds the guard as part of the refactor.
