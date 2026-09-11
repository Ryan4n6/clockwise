#pragma once
#include <stdint.h>
#include <string.h>

// Scroll geometry for canvas text elements, kept free of any Arduino header so
// it can be unit-tested on the host with `pio test -e native`. The clockface
// owns all drawing; this file owns all arithmetic.

enum ScrollMode {
  SCROLL_NONE   = 0,   // never move, truncate at the box edge
  SCROLL_ONCE   = 1,   // run through once on arrival, then park at the start
  SCROLL_LOOP   = 2,   // ping-pong forever
  SCROLL_TICKER = 3,   // Times Square: one direction, wrapping, never stops
};

// How many pixel-steps to hold at the far end of a SCROLL_ONCE pass before
// snapping back. 25 steps at the default 40 ms/step is a one second pause,
// which is about how long it takes to read the tail of a line.
#define SCROLL_HOLD_STEPS 25

// The blank run between the tail of the string and the head of its repeat, in
// pixels (clockwise#18). Without it the two copies butt together and the line
// reads as one run-on word at the seam: `...WANDERERSv WOLVERHAMPTON...`. Eight
// pixels is about two picopixel characters, wide enough to read as a break and
// narrow enough that the wrap never shows an empty box.
#define SCROLL_TICKER_GAP 8

// The distance between one copy of the string and the next. The offset cycles
// over exactly this, which is what makes the wrap seamless: at -pitch the second
// copy sits precisely where the first one started, so the frame at -pitch and the
// frame at 0 are the same picture and the reset is invisible.
inline uint16_t scrollTickerPitch(uint16_t textW)
{
  const uint32_t pitch = (uint32_t)textW + SCROLL_TICKER_GAP;
  return (pitch > 32767UL) ? 32767 : (uint16_t)pitch;
}

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

  // Both widths are uint16_t, so travel can reach 65535 and the negation below
  // would wrap an int16_t. No real font produces a 32767 px string, but the
  // clamp costs one comparison and removes the question entirely.
  uint32_t travel = (uint32_t)(textW - boxW);
  if (travel > 32767UL) travel = 32767UL;

  const uint32_t step = elapsedMs / scrollMs;

  if (mode == SCROLL_TICKER) {
    // Times Square (clockwise#18). One direction, wrapping, no hold and no end.
    //
    // It cycles over the PITCH and not over `travel`, which is the whole
    // difference. Travel is how far the text has to move for its tail to reach
    // the box edge, and a mode that turns around there is a ping-pong. Pitch is
    // the distance to the next copy of the string, so running the offset over it
    // and drawing the string again one pitch to the right means the head is
    // already entering on the right as the tail leaves on the left, and the
    // frame at -pitch is pixel identical to the frame at 0.
    //
    // The caller has to draw that second copy. See Clockface::drawScroller().
    r.offsetX = -(int16_t)(step % scrollTickerPitch(textW));
    r.done    = false;
    return r;
  }

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

// Maps a document's "scroll" string to a mode. Anything unrecognised, missing,
// or null means "do not move", because a typo in a canvas document should
// produce a boring panel and never a moving one.
inline uint8_t scrollModeFromName(const char *name)
{
  if (name == nullptr)             return SCROLL_NONE;
  if (strcmp(name, "once") == 0)   return SCROLL_ONCE;
  if (strcmp(name, "loop") == 0)   return SCROLL_LOOP;
  if (strcmp(name, "ticker") == 0) return SCROLL_TICKER;
  return SCROLL_NONE;
}
