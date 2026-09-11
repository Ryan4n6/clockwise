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
#include "CWTextMeasure.h"
#include "DbgUdp.h"
#include <TextScroll.h>

#define CLOCKFACE_NAME "cw-cf-0x07"

// Fallback re-fetch cadence when a canvas document omits "refresh" (30 min).
// Every legacy canvas gets this for free; a document can ask for its own rate.
#define CANVAS_DEFAULT_REFRESH_MS 1800000UL
// Floor the requested rate so a bad/zero "refresh" can't turn the panel into a
// tight HTTP loop against the origin. Lowered from 60000 for multi-face
// rotation: a face cannot be shown for less time than the fetch interval, so
// the old floor capped rotation granularity at one minute.
#define CANVAS_MIN_REFRESH_MS 15000UL

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

// The off-screen surface text is composed on before being blitted into its box.
// 64 wide because that is the panel; 32 tall covers the tallest bundled font
// with room to spare. 1 bit per pixel, so the whole thing is 256 bytes.
#define TEXTBOX_CANVAS_W 64
#define TEXTBOX_CANVAS_H 32

// One overflowing text element being animated. Position and geometry are copied
// out of the document at setup time rather than re-read every frame, so a frame
// costs no JSON traversal.
struct TextScroller {
  uint8_t  elementIndex;   // index into doc["setup"]
  int16_t  x, y;           // element origin on the panel
  uint16_t boxW;           // clip width; text wider than this scrolls
  uint16_t textW;          // measured extent of the full string, unwrapped
  uint16_t scrollMs;
  uint8_t  mode;
  uint32_t startMs;
  bool     done;
  int16_t  lastOffsetX;    // skip the redraw when the offset has not changed
};

// The panel rectangle one text element last inked (clockwise#16).
//
// renderText()'s unclipped path erased the box of the string it was ABOUT to
// draw, which is right only while strings never get narrower. `4:30` inks 35px
// and `4:31` inks 31px in hour8pt7b, because `1` is 4px narrower than `0`, so
// the last 4 columns of the old `0` were never erased and sat lit beside the new
// `1`. Ryan watched it happen on the wall at 4:30 to 4:31.
//
// The cure is memory: erase where the glyphs ARE before drawing where they are
// going. One of these per element, filled in after every draw.
struct TextExtent {
  int16_t  x, y;
  uint16_t w, h;
  bool     valid;
};

// Enough for any document that fits JSON_BUFFER_BYTES. The moon face draws 4
// elements and the busiest rotation face draws 5; 32 is room to be wrong by a
// factor of six. An element past this simply keeps the old behaviour, which is
// the behaviour every canvas had before this existed.
#define MAX_TEXT_EXTENTS 32

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

  // Last rendered document's "etag". With refresh capped at 5 minutes for
  // rotation, the panel re-fetches 288 times a day, and clockfaceSetup() opens
  // with a full-screen fillRect. Repainting an unchanged document would flash
  // the panel every 5 minutes all night. Empty means "no etag seen yet".
  String _lastEtag;

  // Last rendered document's "face". Rotation decision 3: a scroll replays on a
  // change of face only, never on a content change to the face already showing.
  // Without this the weather face restarts every scroller each time the
  // temperature ticks a degree, because that changes the etag while the face
  // stays the same. Empty means "no face seen yet", which is also what a
  // document carrying no "face" field gets, so a legacy canvas keeps today's
  // behaviour exactly: rebuild everything, replay every scroll.
  String _lastFace;

  // Scroller state carried across a same-face repaint, handed from
  // refetchCanvas() to buildScrollers() through clockfaceSetup(), which has no
  // parameters to thread it through. Empty on every other path.
  std::vector<TextScroller> _carriedScrollers;

  void refetchCanvas();

  void setFont(const char *fontName);
  uint16_t measureTextWidth(const char *content, const char *fontName);
  void drawTextBoxed(int16_t x, int16_t y, const char *content,
                     const char *fontName, uint16_t fg, uint16_t bg,
                     uint16_t boxW, int16_t offsetX);
  void buildScrollers();
  void scrollLoop();
  void drawScroller(TextScroller &s, int16_t offsetX);
  bool deserializeDefinition();
  void clockfaceSetup();
  void clockfaceLoop();
  void renderElements(JsonArrayConst elements);
  void renderText(String text, JsonVariantConst value, uint8_t elementIndex);
  void createSprites();
  void refreshDateTime();
  void drawSplashScreen(uint16_t color, const char *msg);
  void handleSpriteAnimation(std::shared_ptr<CustomSprite> &sprite);
  void handleSpriteMovement(std::shared_ptr<CustomSprite> &sprite);

  std::vector<std::shared_ptr<CustomSprite>> sprites;
  std::vector<TextScroller> scrollers;

  // Indexed by position in doc["setup"], invalidated whenever that array can
  // mean something different, which is any document change. See renderText().
  TextExtent _lastExtent[MAX_TEXT_EXTENTS] = {};

public:
  // Public and static only so measureTextBoundsHook(), a free function handed to
  // the web server as a plain function pointer, can reach it (clockwise#17).
  static void setFontOn(Adafruit_GFX *target, const char *fontName);

  Clockface(Adafruit_GFX *display);
  void setup(CWDateTime *dateTime);
  void update();
};
