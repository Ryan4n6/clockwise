
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

void Clockface::setup(CWDateTime *dateTime)
{
  this->_dateTime = dateTime;
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

  // Drop sprites BEFORE the fetch. deserializeDefinition() parses straight into
  // the shared `doc`, so the instant it runs every sprite's index points at the
  // old document. On success clockfaceSetup() rebuilds them; on failure `doc` is
  // left clobbered and a surviving sprite would hand renderImage() a nullptr
  // (handleSpriteAnimation) -> LoadProhibited panic.
  sprites.clear();

  if (deserializeDefinition())
  {
    clockfaceSetup();
    DBG("CF11 canvas refetch OK, repainted");
  }
  else
  {
    // deserializeDefinition() already drew its own error splash. The last good
    // static elements stay on the panel, which beats a blank one or a reboot.
    DBG("CF11 canvas refetch FAILED, keeping last frame");
  }
}

void Clockface::setFont(const char *fontName)
{

  if (strcmp(fontName, "picopixel") == 0)
  {
    Locator::getDisplay()->setFont(&Picopixel);
  }
  else if (strcmp(fontName, "square") == 0)
  {
    Locator::getDisplay()->setFont(&atariFont);
  }
  else if (strcmp(fontName, "big") == 0)
  {
    Locator::getDisplay()->setFont(&hour8pt7b);
  }
  else if (strcmp(fontName, "medium") == 0)
  {
    Locator::getDisplay()->setFont(&minute7pt7b);
  }
  else
  {
    Locator::getDisplay()->setFont();
  }
}

void Clockface::renderText(String text, JsonVariantConst value)
{
  int16_t x1, y1;
  uint16_t w, h;

  setFont(value["font"].as<const char *>());

  Locator::getDisplay()->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);

  // BG Color
  Locator::getDisplay()->fillRect(
      value["x"].as<const uint16_t>() + x1,
      value["y"].as<const uint16_t>() + y1,
      w,
      h,
      value["bgColor"].as<const uint16_t>());

  Locator::getDisplay()->setTextColor(value["fgColor"].as<const uint16_t>());
  Locator::getDisplay()->setCursor(value["x"].as<const uint16_t>(), value["y"].as<const uint16_t>());
  Locator::getDisplay()->print(text);
}

void Clockface::refreshDateTime()
{

  JsonArrayConst elements = doc["setup"].as<JsonArrayConst>();
  for (JsonVariantConst value : elements)
  {
    const char *type = value["type"].as<const char *>();

    if (strcmp(type, "datetime") == 0)
    {
      renderText(_dateTime->getFormattedTime(value["content"].as<const char *>()), value);
    }
  }
}

void Clockface::clockfaceSetup()
{

  // Clear screen
  Locator::getDisplay()->fillRect(0, 0, 64, 64, doc["bgColor"].as<const uint16_t>());

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

void Clockface::clockfaceLoop() {
    if (sprites.empty()) {
        return;
    }

    for (auto& sprite : sprites) {
        handleSpriteAnimation(sprite);
    }
}

void Clockface::renderElements(JsonArrayConst elements)
{
  for (JsonVariantConst value : elements)
  {
    const char *type = value["type"].as<const char *>();

    if (strcmp(type, "text") == 0)
    {
      renderText(value["content"].as<const char *>(), value);
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
