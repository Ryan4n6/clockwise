#pragma once

#include <Arduino.h>

#include <Adafruit_GFX.h>
#include <Locator.h>
#include <ArduinoJson.h>
#include <vector>
#include <CWPreferences.h>
#include <StatusController.h>

// Commons
#include "IClockface.h"
#include "Icons.h"
#include "picopixel.h"
#include "fonts/atari.h"
#include "fonts/hour8pt7b.h"
#include "fonts/minute7pt7b.h"
#include "PNGRender.h"
#include "CustomSprite.h"
#include "CWHttpClient.h"
#include "DbgUdp.h"

#define CLOCKFACE_NAME "cw-cf-0x07"

// Fallback re-fetch cadence when a canvas document omits "refresh" (30 min).
// Every legacy canvas gets this for free; a document can ask for its own rate.
#define CANVAS_DEFAULT_REFRESH_MS 1800000UL
// Floor the requested rate so a bad/zero "refresh" can't turn the panel into a
// tight HTTP loop against the origin.
#define CANVAS_MIN_REFRESH_MS 60000UL

const uint8_t CW_ICON_CANVAS[] PROGMEM = { 
	0x00, 0x0e, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 
	0x00, 0x3f, 0x80, 0x00, 0x00, 0x40, 0x40, 0x00, 0x1f, 0xc0, 0x7f, 0x00, 0x20, 0x3f, 0x80, 0x80, 
	0x20, 0x00, 0x00, 0x80, 0x20, 0x00, 0x00, 0x80, 0x20, 0x00, 0x00, 0x80, 0x20, 0x00, 0x00, 0x80, 
	0x20, 0x00, 0x00, 0x80, 0x20, 0x00, 0x00, 0x80, 0x20, 0x00, 0x00, 0x80, 0x20, 0x00, 0x00, 0x80, 
	0x20, 0x00, 0x00, 0x80, 0x20, 0x00, 0x00, 0x80, 0x20, 0x00, 0x00, 0x80, 0x20, 0x00, 0x00, 0x80, 
	0x20, 0x00, 0x00, 0x80, 0x7f, 0xff, 0xff, 0xc0, 0x80, 0x00, 0x00, 0x20, 0x7f, 0xff, 0xff, 0xc0, 
	0x0e, 0x1f, 0x0e, 0x00, 0x0e, 0x1f, 0x0e, 0x00, 0x1f, 0xff, 0xff, 0x00, 0x1e, 0x1f, 0x0f, 0x00, 
	0x3f, 0xff, 0xff, 0x80, 0x3c, 0x1f, 0x07, 0x80, 0x3c, 0x1f, 0x07, 0x80, 0x18, 0x0e, 0x03, 0x00
};

class Clockface : public IClockface
{
private:
  Adafruit_GFX *_display;
  CWDateTime *_dateTime;
  uint16_t delay;

  // Canvas re-fetch (#11). The clockface used to GET its document exactly once,
  // in setup(), and update() only ever re-rendered "datetime" elements. A canvas
  // whose content is generated server-side (the moon face: age, illumination,
  // phase name, moon image) therefore froze at whatever it downloaded at boot.
  // _refreshMs comes from the document's own "refresh" field so the server owns
  // its cadence. It is uint32_t on purpose: "delay" above is uint16_t for sprite
  // compatibility and silently truncates anything over 65535 ms.
  unsigned long _lastFetchMillis = 0;
  uint32_t      _refreshMs       = CANVAS_DEFAULT_REFRESH_MS;

  void refetchCanvas();

  void setFont(const char *fontName);
  bool deserializeDefinition();
  void clockfaceSetup();
  void clockfaceLoop();
  void renderElements(JsonArrayConst elements);
  void renderText(String text, JsonVariantConst value);
  void createSprites();
  void refreshDateTime();
  void drawSplashScreen(uint16_t color, const char *msg);
  void handleSpriteAnimation(std::shared_ptr<CustomSprite> &sprite);
  void handleSpriteMovement(std::shared_ptr<CustomSprite> &sprite);

  std::vector<std::shared_ptr<CustomSprite>> sprites;

public:
  Clockface(Adafruit_GFX *display);
  void setup(CWDateTime *dateTime);
  void update();
};
