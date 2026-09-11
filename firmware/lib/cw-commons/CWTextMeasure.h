#pragma once

#include <stdint.h>

// The seam for GET /measure (clockwise#17).
//
// Nothing in cw-commons can measure text: the font tables and the name to font
// mapping live in whichever clockface is compiled in, and commons is deliberately
// graphics free. So the web server asks, and a clockface that can answer
// registers itself here at setup(). One that cannot leaves the hook null and the
// endpoint replies 501.
//
// WHY ITS OWN HEADER. The obvious place for this is CWWebServer.h, and putting it
// there would make the clockface include the web server to reach it: WiFi, the
// settings page's several kilobytes of HTML, and a WiFiServer at file scope that
// only links while exactly one .cpp includes it. This header is a typedef and an
// accessor, so the clockface takes on nothing to answer a question about fonts.
//
// `w`/`h` are the UNWRAPPED extent, which is what a server computing an element's
// x and w is modelling. `wrapW`/`wrapH` are the same string measured through the
// display, whose text wrap Adafruit_GFX defaults to on. For anything that fits
// the panel the two agree; both are reported so the day they stop agreeing shows
// up as a number rather than as a photograph of a clipped digit.
//
// Returns false for a font name the clockface does not know.
typedef bool (*CWMeasureTextFn)(const char *fontName, const char *text,
                                int16_t *x1, int16_t *y1, uint16_t *w, uint16_t *h,
                                uint16_t *wrapW, uint16_t *wrapH);

// One hook for the whole program. A function local static inside an inline
// function, which is the same rule ClockwiseWebServer::getInstance() already
// relies on to be a singleton across translation units.
inline CWMeasureTextFn &cwMeasureTextHook()
{
  static CWMeasureTextFn fn = nullptr;
  return fn;
}
