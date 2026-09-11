
#include "Clockface.h"

unsigned long lastMillis = 0;

// TODO document size
// DIAG: shrunk 32768 -> 6144 to test whether the large static-init heap alloc
// is what faults the Canvas clockface before wifi. Moon JSON is ~1.2KB.
static DynamicJsonDocument doc(6144);

Clockface::Clockface(Adafruit_GFX *display)
{
  _display = display;
  Locator::provide(display);
}

// GET /measure's answer (clockwise#17). Defined further down, next to the other
// measuring code, because it needs the text canvas and the font mapping and both
// are declared below this point.
static bool measureTextBoundsHook(const char *fontName, const char *text,
                                  int16_t *x1, int16_t *y1, uint16_t *w, uint16_t *h,
                                  uint16_t *wrapW, uint16_t *wrapH);

void Clockface::setup(CWDateTime *dateTime)
{
  this->_dateTime = dateTime;
  cwMeasureTextHook() = &measureTextBoundsHook;
  drawSplashScreen(0xFFE0, "Downloading");

  if (deserializeDefinition()) {
    DBG("CF7 deser OK");
    clockfaceSetup();
    // Seed the re-fetch clock from the boot fetch so the first refresh lands one
    // full interval later, not immediately (#11).
    _lastFetchMillis = millis();
    DBG("CF9 clockfaceSetup done");
  } else {
    DBG("CF7 deser FAILED");
  }
}

void Clockface::drawSplashScreen(uint16_t color, const char *msg) {
  
  Locator::getDisplay()->fillRect(0, 0, 64, 64, 0);
  Locator::getDisplay()->drawBitmap(19, 18, CW_ICON_CANVAS, 27, 32, color);
  
  StatusController::getInstance()->printCenter("- Canvas -", 7);
  StatusController::getInstance()->printCenter(msg, 61);
}

void Clockface::update()
{
  // Render animation
  clockfaceLoop();

  // Update Date/Time - Using a fixed interval (1000 milliseconds)
  if (millis() - lastMillis >= 1000)
  {
    refreshDateTime();
    lastMillis = millis();
  }

  // Re-pull the canvas document (#11). refreshDateTime() above only repaints
  // "datetime" elements, so a server-generated canvas (moon age, illumination,
  // phase name, moon image) would otherwise show its boot-time values forever.
  // Unsigned subtraction, so this stays correct across the millis() rollover.
  if (millis() - _lastFetchMillis >= _refreshMs)
  {
    refetchCanvas();
  }
}

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

    // Rotation decision 3: a scroll replays on a change of FACE, not on a change
    // of content. The weather face ticks a degree, the etag changes, the face
    // does not, and restarting every scroller there would yank a half-read line
    // back to its start every few minutes (clockwise#15).
    //
    // Matching carried state by elementIndex is sound precisely because the face
    // is unchanged: the same face means the same template, so the element layout
    // is stable and only the bound values differ. A face that hid an element
    // conditionally would shift indexes underneath an unchanged face id and need
    // a stronger key; nothing does that yet, and the guards below check the
    // geometry as well.
    const char *face = doc["face"] | "";
    const bool sameFace = (face[0] != '\0' && _lastFace == face);
    _lastFace = face;

    _carriedScrollers.clear();
    if (sameFace) {
      _carriedScrollers.swap(scrollers);
      DBG((String("CF13 same face, carrying ") + String((int)_carriedScrollers.size()) + " scroller(s)").c_str());
    }

    // Content changed (or the document carries no etag, so we cannot know).
    // Drop sprites before rebuilding: their indexes referred to the previous
    // document and clockfaceSetup() recreates them from the new one. Scrollers
    // hold an elementIndex into that same document, for the same reason.
    sprites.clear();
    scrollers.clear();
    clockfaceSetup();
    _carriedScrollers.clear();
    DBG("CF11 canvas refetch OK, repainted");
  }
  else
  {
    // deserializeDefinition() already drew its own error splash. A failed parse
    // leaves the shared `doc` clobbered, so any surviving sprite would index
    // into it and hand renderImage() a nullptr -> LoadProhibited panic.
    sprites.clear();
    scrollers.clear();
    _carriedScrollers.clear();
    _lastEtag = "";   // force a repaint on the next good fetch
    _lastFace = "";   // and a full rebuild, so nothing is carried across a gap
    DBG("CF11 canvas refetch FAILED, keeping last frame");
  }
}

// Font selection against an arbitrary surface. The panel and the off-screen
// text canvas both need it, and this Adafruit_GFX has no getFont() to read the
// current selection back with, so the caller names the font every time.
void Clockface::setFontOn(Adafruit_GFX *target, const char *fontName)
{
  // The original passed fontName straight to strcmp, so a text element that
  // omitted "font" dereferenced null and panicked the panel.
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

// The shared off-screen surface for boxed text. A function-local static so the
// 256 byte malloc happens on first use rather than during static init: this
// file already carries a DIAG note about a static-init heap allocation faulting
// the Canvas clockface before wifi comes up, and there is no reason to re-open
// that question for a scratch buffer.
//
// Text wrap is off on it, permanently and deliberately. Adafruit_GFX defaults
// wrap to true (Adafruit_GFX.cpp:117) and honours it in BOTH charBounds (the
// measure path) and write (the draw path) against the surface width. Leaving it
// on means a 100 px string measured on a 64 px surface reports back as 64 px
// wide and two lines tall, so nothing ever looks like it overflows, no scroller
// is ever built, and the feature silently does nothing.
static GFXcanvas1 &textCanvas()
{
  static GFXcanvas1 c(TEXTBOX_CANVAS_W, TEXTBOX_CANVAS_H);
  static bool configured = false;
  if (!configured) { c.setTextWrap(false); configured = true; }
  return c;
}

// True unwrapped pixel width of `content` in `fontName`.
uint16_t Clockface::measureTextWidth(const char *content, const char *fontName)
{
  if (content == nullptr) return 0;

  GFXcanvas1 &canvas = textCanvas();
  setFontOn(&canvas, fontName);

  int16_t bx, by;
  uint16_t bw, bh;
  canvas.getTextBounds(content, 0, 0, &bx, &by, &bw, &bh);
  return bw;
}

// What the device thinks a string measures, for GET /measure (clockwise#17).
//
// The name to font mapping is validated here rather than leaned on, because
// setFontOn() falls through to the built in 5x7 for anything it does not
// recognise. That fallback is right for rendering a document with a typo in it
// and wrong for an endpoint whose entire job is to answer truthfully: a server
// comparing its tables against a misspelled font would be told the typo is fine.
static bool measureTextBoundsHook(const char *fontName, const char *text,
                                  int16_t *x1, int16_t *y1, uint16_t *w, uint16_t *h,
                                  uint16_t *wrapW, uint16_t *wrapH)
{
  if (fontName == nullptr || text == nullptr) return false;

  // Exactly the four names setFontOn() recognises. Anything else is a 404 rather
  // than a measurement of the built in 5x7, which is what setFontOn() would
  // quietly hand back. Note for whoever pins these: the two sides disagree on
  // what an UNKNOWN name means. The worker falls through to Picopixel and the
  // firmware falls through to the 5x7, so a document with a misspelled font
  // already renders in a font the server did not measure. No face emits one, and
  // this endpoint refusing to answer keeps that gap visible.
  if (strcmp(fontName, "picopixel") != 0 && strcmp(fontName, "square") != 0 &&
      strcmp(fontName, "big") != 0 && strcmp(fontName, "medium") != 0)
  {
    return false;
  }

  // Unwrapped, off the same 1 bit canvas measureTextWidth() uses. This is the
  // number a server placing an element is modelling.
  GFXcanvas1 &canvas = textCanvas();
  Clockface::setFontOn(&canvas, fontName);
  canvas.getTextBounds(text, 0, 0, x1, y1, w, h);

  // And again through the display, whose text wrap Adafruit_GFX leaves ON by
  // default and which this clockface never turns off. That is the path
  // renderText() takes for an element carrying no box, so it is the one that can
  // surprise, and it agrees with the above for anything narrower than the panel.
  //
  // Borrowing the display's font here is safe: every render sets the font per
  // element before drawing, so nothing downstream reads what this left behind.
  int16_t wx, wy;
  Clockface::setFontOn(Locator::getDisplay(), fontName);
  Locator::getDisplay()->getTextBounds(text, 0, 0, &wx, &wy, wrapW, wrapH);
  return true;
}

// Draw `content` into a boxW-wide window whose left edge is at `x`, shifted left
// by `offsetX` (0 or negative). Used for both static truncation and scrolling,
// so the two share one clipping path.
//
// Composing into a 1-bit canvas is what buys the right-edge clip. Drawing at a
// negative x clips on the left for free because writePixel bounds-checks, but
// nothing clips the right, and an overflowing string would paint straight over
// whatever element sits beside it.
//
// The blit hands drawBitmap the CANVAS width, not the box width. drawBitmap
// recomputes its row stride as (w + 7) / 8 from the width it is given
// (Adafruit_GFX.cpp:1012) while GFXcanvas1 allocated its rows at
// (TEXTBOX_CANVAS_W + 7) / 8 (Adafruit_GFX.cpp:2027). Passing any other width
// makes it read every row at the wrong offset, which renders as diagonal hash.
// So: zero the canvas columns past the box, blit the full canvas width with the
// transparent overload, and erase the box with a fillRect first because a
// transparent blit paints only set bits and would leave the last frame behind.
void Clockface::drawTextBoxed(int16_t x, int16_t y, const char *content,
                              const char *fontName, uint16_t fg, uint16_t bg,
                              uint16_t boxW, int16_t offsetX)
{
  if (content == nullptr || boxW == 0) return;

  GFXcanvas1 &canvas = textCanvas();
  if (canvas.getBuffer() == nullptr) return;   // the 256 byte malloc failed

  const uint16_t box = (boxW > TEXTBOX_CANVAS_W) ? TEXTBOX_CANVAS_W : boxW;

  setFontOn(&canvas, fontName);

  int16_t bx, by;
  uint16_t bw, bh;
  canvas.getTextBounds(content, 0, 0, &bx, &by, &bw, &bh);
  const uint16_t h = (bh > TEXTBOX_CANVAS_H) ? TEXTBOX_CANVAS_H : bh;
  if (h == 0) return;                          // empty string, nothing to draw

  canvas.fillScreen(0);
  canvas.setTextColor(1);
  // getTextBounds returns offsets relative to the cursor, so subtract them to
  // land the glyphs inside the canvas instead of above its top edge.
  canvas.setCursor(offsetX - bx, -by);
  canvas.print(content);

  // Anything past the box must be blank, because the blit below is canvas-wide.
  if (box < TEXTBOX_CANVAS_W)
  {
    canvas.fillRect(box, 0, TEXTBOX_CANVAS_W - box, TEXTBOX_CANVAS_H, 0);
  }

  // Erase then draw, the same shape handleSpriteMovement() already uses.
  Locator::getDisplay()->fillRect(x, y + by, box, h, bg);
  Locator::getDisplay()->drawBitmap(x, y + by, canvas.getBuffer(),
                                    TEXTBOX_CANVAS_W, h, fg);
}

void Clockface::renderText(String text, JsonVariantConst value, uint8_t elementIndex)
{
  int16_t x1, y1;
  uint16_t w, h;

  // An element that declares a box width gets the clipped path, whether or not
  // it also asked to scroll. Without this a string too wide for its box is
  // painted full length here, once, over the top of its neighbours, and the
  // scroller only ever repaints inside the box so the spill stays on the panel
  // forever. It is also what makes SCROLL_NONE mean what its comment says it
  // means: truncate at the box edge.
  //
  // Elements with no "w" keep the original unclipped path byte for byte, so
  // every existing canvas document (the moon face included) renders exactly as
  // it did before.
  if (!value["w"].isNull())
  {
    drawTextBoxed(value["x"].as<int16_t>(),
                  value["y"].as<int16_t>(),
                  text.c_str(),
                  value["font"].as<const char *>(),
                  value["fgColor"].as<const uint16_t>(),
                  value["bgColor"].as<const uint16_t>(),
                  value["w"].as<uint16_t>(),
                  0);
    return;
  }

  setFont(value["font"].as<const char *>());

  Locator::getDisplay()->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);

  const int16_t ox = value["x"].as<const int16_t>();
  const int16_t oy = value["y"].as<const int16_t>();
  const uint16_t bg = value["bgColor"].as<const uint16_t>();

  // ERASE WHERE THE GLYPHS ARE, THEN WHERE THEY ARE GOING (clockwise#16).
  //
  // This used to erase only the second of those, which is correct exactly as
  // long as no string is ever narrower than the one it replaces. `4:30` inks
  // 35px and `4:31` inks 31px, because `1` is 4px narrower than `0` in
  // hour8pt7b, so the last 4 columns of the old zero were never cleared and
  // stayed lit next to the new one. Ryan photographed it at 4:30 to 4:31.
  //
  // The worker mitigated it by giving every clock a `w`, which routes it down
  // drawTextBoxed() instead and clears a fixed box. That is still the right
  // thing for a text element whose content varies, because a box is stable
  // geometry that the server can reason about. This is the cure underneath it:
  // an element that carries no `w` at all is now also safe, which is every
  // legacy canvas document ever written, none of which can be revised.
  //
  // Both rectangles, not just the previous one. The first draw has no previous
  // extent, and a string that GREW needs the new area cleared too.
  if (elementIndex < MAX_TEXT_EXTENTS && _lastExtent[elementIndex].valid)
  {
    const TextExtent &p = _lastExtent[elementIndex];
    Locator::getDisplay()->fillRect(p.x, p.y, p.w, p.h, bg);
  }
  Locator::getDisplay()->fillRect(ox + x1, oy + y1, w, h, bg);

  Locator::getDisplay()->setTextColor(value["fgColor"].as<const uint16_t>());
  Locator::getDisplay()->setCursor(ox, oy);
  Locator::getDisplay()->print(text);

  if (elementIndex < MAX_TEXT_EXTENTS)
  {
    _lastExtent[elementIndex] = { (int16_t)(ox + x1), (int16_t)(oy + y1), w, h, true };
  }
}

void Clockface::refreshDateTime()
{

  JsonArrayConst elements = doc["setup"].as<JsonArrayConst>();
  uint8_t i = 0;
  for (JsonVariantConst value : elements)
  {
    const char *type = value["type"].as<const char *>();

    if (strcmp(type, "datetime") == 0)
    {
      renderText(_dateTime->getFormattedTime(value["content"].as<const char *>()), value, i);
    }
    i++;
  }
}

void Clockface::clockfaceSetup()
{

  // Clear screen
  Locator::getDisplay()->fillRect(0, 0, 64, 64, doc["bgColor"].as<const uint16_t>());

  // And forget every remembered text extent with it (clockwise#16). These are
  // keyed by position in doc["setup"], and a new document can put a different
  // element at the same index. Erasing element 2's OLD rectangle on a document
  // where element 2 has moved would rub out whatever element 1 just drew there.
  // The full-screen clear above means there is nothing left to erase anyway.
  for (uint8_t i = 0; i < MAX_TEXT_EXTENTS; i++) _lastExtent[i].valid = false;

  delay = doc["delay"].as<const uint16_t>();

  // "refresh" is the document's own re-fetch cadence in ms (#11). Read as
  // uint32_t: `delay` above is uint16_t and truncates anything over 65535, which
  // is why the moon worker's delay:1800000 was silently arriving as 30528.
  // Clamp up to CANVAS_MIN_REFRESH_MS so a missing/zero/typo'd value can never
  // become a tight fetch loop.
  _refreshMs = doc["refresh"] | CANVAS_DEFAULT_REFRESH_MS;
  if (_refreshMs < CANVAS_MIN_REFRESH_MS) _refreshMs = CANVAS_MIN_REFRESH_MS;

  // Draw static elements
  renderElements(doc["setup"].as<JsonArrayConst>());

  // Draw Date/Time
  refreshDateTime();

  // Create sprites
  createSprites();

  // Collect the text elements that overflow their box. Must run after
  // renderElements() above, which has already drawn every element's first
  // frame (clipped, for anything carrying a "w").
  buildScrollers();
}

void Clockface::createSprites()
{
  JsonArrayConst elements = doc["loop"].as<JsonArrayConst>();
  uint8_t width = 0;
  uint8_t height = 0;

  for (JsonVariantConst value : elements)
  {
    const char *type = value["type"].as<const char *>();

    if (strcmp(type, "sprite") == 0)
    {
      uint8_t ref = value["sprite"].as<const uint8_t>();

      std::shared_ptr<CustomSprite> s = std::make_shared<CustomSprite>(value["x"].as<const int8_t>(), value["y"].as<const int8_t>());

      getImageDimensions(doc["sprites"][ref][0]["image"].as<const char *>(), width, height);

      s.get()->_spriteReference = value["sprite"].as<const uint8_t>();
      s.get()->_totalFrames = doc["sprites"][ref].size();
      s.get()->setDimensions(width, height);
      sprites.push_back(s);
    }
  }
}

void Clockface::handleSpriteAnimation(std::shared_ptr<CustomSprite>& sprite) {
    uint8_t totalFrames = sprite->_totalFrames;
    uint32_t loopDelay = doc["loop"][sprite->_spriteReference]["loopDelay"].as<uint32_t>() ?: delay;
    uint16_t frameDelay = doc["loop"][sprite->_spriteReference]["frameDelay"].as<uint16_t>() ?: delay;

    if (millis() - sprite->_lastMillisSpriteFrames >= frameDelay && sprite->_currentFrameCount < totalFrames) {
        sprite->incFrame();

        // handle sprite movement
        handleSpriteMovement(sprite);

        // Render the frame of the sprite
        renderImage(doc["sprites"][sprite->_spriteReference][sprite->_currentFrame]["image"].as<const char *>(), sprite->getX(), sprite->getY());

        sprite->_currentFrameCount += 1;
        sprite->_lastMillisSpriteFrames = millis();
    }

    if (millis() - sprite->_lastResetTime >= loopDelay) {
        unsigned long currentMillis = millis();
        unsigned long currentSecond = _dateTime->getSecond();

        if ((currentSecond * 1000) % loopDelay == 0) {
            sprite->_currentFrameCount = 0;
            sprite->_lastResetTime = currentMillis;
        }
    }
}

void Clockface::handleSpriteMovement(std::shared_ptr<CustomSprite>& sprite) {
    unsigned long moveStartTime = doc["loop"][sprite->_spriteReference]["moveStartTime"].as<unsigned long>() ?: 1;
    unsigned long moveDuration = doc["loop"][sprite->_spriteReference]["moveDuration"].as<unsigned long>() ?: 0;
    int8_t moveInitialX = doc["loop"][sprite->_spriteReference]["x"].as<int8_t>() ?:0;
    int8_t moveInitialY = doc["loop"][sprite->_spriteReference]["y"].as<int8_t>() ?: 0;
    int8_t moveTargetX = doc["loop"][sprite->_spriteReference]["moveTargetX"].as<int8_t>() ?: -1;
    int8_t moveTargetY = doc["loop"][sprite->_spriteReference]["moveTargetY"].as<int8_t>() ?: -1;
    bool shouldReturnToOrigin = doc["loop"][sprite->_spriteReference]["shouldReturnToOrigin"].as<bool>() ?: false;

    // Check if the sprite is moving
    if (sprite->isMoving()) {
        unsigned long currentTime = millis();
        unsigned long elapsedTime = currentTime - sprite->_moveStartTime;
        float progress = (static_cast<float>(elapsedTime) / sprite->_moveDuration);

        int8_t oldX = sprite->getX();
        int8_t oldY = sprite->getY();
        int8_t newX = sprite->lerp(sprite->_moveInitialX, sprite->_moveTargetX, progress);
        int8_t newY = sprite->lerp(sprite->_moveInitialY, sprite->_moveTargetY, progress);
        int8_t originX = min(oldX, newX);
        int8_t originY = min(oldY, newY);
        int8_t drawWidth = sprite->getWidth() + max(oldX, newX) - originX;
        int8_t drawHeight = sprite->getHeight() + max(oldY, newY) - originY;

        // Erase the previous position
        Locator::getDisplay()->fillRect(
            originX,
            originY,
            drawWidth,
            drawHeight,
            doc["bgColor"].as<const uint16_t>());

        if (progress <= 1) {
            // Update the sprite's position
            sprite->setX(newX);
            sprite->setY(newY);

        } else if (sprite->shouldReturnToOrigin()) {
            // Movement is complete
            sprite->setX(sprite->_moveTargetX);
            sprite->setY(sprite->_moveTargetY);

            if (!sprite->_isReversing) {
                sprite->reverseMoving(moveInitialX, moveInitialY);
            }
        } else {
            sprite->stopMoving();
        }
    }

    if ((moveDuration > 0 && (moveTargetX > -1 || moveTargetY > -1)) && (millis() - sprite->_lastResetMoveTime >= moveStartTime)) {
        unsigned long currentMillis = millis();
        unsigned long currentSecond = _dateTime->getSecond();

        if ((currentSecond * 1000) % moveStartTime == 0) {
            sprite->_lastResetMoveTime = currentMillis;
            sprite->startMoving(moveTargetX, moveTargetY, moveDuration, shouldReturnToOrigin);
        }
    }
}

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
    s.x = value["x"].as<int16_t>();
    s.y = value["y"].as<int16_t>();
    // Default the box to "the rest of the panel", which is what a face means
    // when it does not say. Guard against x beyond the panel.
    s.boxW     = value["w"] | (uint16_t)((s.x < 64) ? (64 - s.x) : 0);
    s.scrollMs = value["scrollMs"] | (uint16_t)40;
    s.mode     = mode;
    s.textW    = measureTextWidth(content, value["font"].as<const char *>());

    // Text that fits is not a scroller at all. renderElements() already drew it
    // statically and it must not be re-drawn every frame.
    if (s.textW <= s.boxW) { idx++; continue; }

    s.startMs     = millis();
    s.done        = false;

    // Carry the scroll's position in time across a same-face repaint. Matching
    // on the geometry as well as the index means a template that moved or
    // resized this element replays rather than resuming part way through a line
    // that is no longer in the same place. An element with no match, on a face
    // that is otherwise the same, is new text and gets a fresh start, which is
    // the replay rotation decision 3 asks for.
    for (const TextScroller &prev : _carriedScrollers) {
      if (prev.elementIndex == s.elementIndex && prev.mode == s.mode &&
          prev.x == s.x && prev.y == s.y && prev.boxW == s.boxW) {
        s.startMs = prev.startMs;
        s.done    = prev.done;
        break;
      }
    }

    // Always the impossible offset, even on a carried scroller: the content
    // behind it just changed, so the box has to be drawn once at whatever offset
    // the carried clock puts it at.
    s.lastOffsetX = 1;
    scrollers.push_back(s);
    idx++;
  }

  if (!scrollers.empty()) {
    DBG((String("CF12 scrollers=") + String((int)scrollers.size())).c_str());
  }
}

void Clockface::drawScroller(TextScroller &s, int16_t offsetX)
{
  JsonVariantConst value = doc["setup"][s.elementIndex];
  const char *content = value["content"].as<const char *>();
  if (content == nullptr) return;

  drawTextBoxed(s.x, s.y, content,
                value["font"].as<const char *>(),
                value["fgColor"].as<const uint16_t>(),
                value["bgColor"].as<const uint16_t>(),
                s.boxW, offsetX);
}

void Clockface::clockfaceLoop() {
    // Sprites and scrollers are independent. The early return here used to be on
    // sprites.empty(), which is true for every face in the rotation design, so
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

void Clockface::renderElements(JsonArrayConst elements)
{
  uint8_t i = 0;
  for (JsonVariantConst value : elements)
  {
    const char *type = value["type"].as<const char *>();

    if (strcmp(type, "text") == 0)
    {
      renderText(value["content"].as<const char *>(), value, i);
    }
    else if (strcmp(type, "fillrect") == 0)
    {
      Locator::getDisplay()->fillRect(
          value["x"].as<const uint16_t>(),
          value["y"].as<const uint16_t>(),
          value["width"].as<const uint16_t>(),
          value["height"].as<const uint16_t>(),
          value["color"].as<const uint16_t>());
    }
    else if (strcmp(type, "rect") == 0)
    {
      Locator::getDisplay()->drawRect(
          value["x"].as<const uint16_t>(),
          value["y"].as<const uint16_t>(),
          value["width"].as<const uint16_t>(),
          value["height"].as<const uint16_t>(),
          value["color"].as<const uint16_t>());
    }
    else if (strcmp(type, "line") == 0)
    {
      Locator::getDisplay()->drawLine(
          value["x"].as<const uint16_t>(),
          value["y"].as<const uint16_t>(),
          value["x1"].as<const uint16_t>(),
          value["y1"].as<const uint16_t>(),
          value["color"].as<const uint16_t>());
    }
    else if (strcmp(type, "image") == 0)
    {
      renderImage(value["image"].as<const char *>(), value["x"].as<const uint8_t>(), value["y"].as<const uint8_t>());
    }
    i++;
  }
}

bool Clockface::deserializeDefinition()
{
  DBG("CF3 deserialize enter");
  if (ClockwiseParams::getInstance()->canvasServer.isEmpty() || ClockwiseParams::getInstance()->canvasFile.isEmpty()) {
    drawSplashScreen(0xC904, "Params werent set");
    return false;
  }


  String server = ClockwiseParams::getInstance()->canvasServer;
  String file = String("/" + ClockwiseParams::getInstance()->canvasFile + ".json");
  // Default to plain HTTP (port 80). A TLS handshake needs a big *contiguous* heap
  // block that the 64x64 HUB75 DMA framebuffers don't leave at 8-bit color depth —
  // which is why the moon face previously had to drop the panel to 4-bit (a
  // posterized, "duotone" moon) just to fetch over HTTPS. Fetching over HTTP removes
  // the handshake entirely, so the panel can run full 8-bit color. Only raw.github
  // needs HTTPS, so keep a secure client for "raw." hosts.
  uint16_t port = 80;

  if (server.startsWith("raw.")) {
    port = 443;
    file = String("/robegamesios/clock-club/main/shared" + file);
  }

  WiFiClient       plainClient;
  WiFiClientSecure secureClient;   // constructed but only allocates on connect()
  WiFiClient      *client = &plainClient;
  if (port == 443) { secureClient.setInsecure(); client = &secureClient; }

  ClockwiseHttpClient::getInstance()->httpGet(client, server.c_str(), file.c_str(), port);
  DBG("CF5 post httpGet");

  DeserializationError error = deserializeJson(doc, *client);
  DBG((String("CF6 deser err=") + error.c_str() + " avail=" + String(client->available())).c_str());
  if (error)
  {
    drawSplashScreen(0xC904, "Error! Check logs");

    Serial.print("deserializeJson() failed: ");
    Serial.println(error.c_str());
    client->stop();
    return false;
  }

  //TODO check if json is valid

  // name/author/version are OPTIONAL in a canvas doc. A missing key makes
  // ArduinoJson's .as<const char*>() return nullptr, and printf("%s", nullptr)
  // does strlen(0x0) -> LoadProhibited panic + reboot loop (this is exactly what
  // crashed the moon face on render when the worker JSON omitted them). Use the
  // `|` default operator so a missing field never reaches printf as a null.
  const char *cfName   = doc["name"]   | "canvas";
  const char *cfAuthor = doc["author"] | "unknown";
  uint16_t    cfVer    = doc["version"] | (uint16_t)0;
  Serial.printf("[Canvas] Building clockface '%s' by %s, version %d\n", cfName, cfAuthor, cfVer);
  client->stop();
  return true;
}
