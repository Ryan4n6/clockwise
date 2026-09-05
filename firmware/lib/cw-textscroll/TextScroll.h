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

  // Both widths are uint16_t, so travel can reach 65535 and the negation below
  // would wrap an int16_t. No real font produces a 32767 px string, but the
  // clamp costs one comparison and removes the question entirely.
  uint32_t travel = (uint32_t)(textW - boxW);
  if (travel > 32767UL) travel = 32767UL;

  const uint32_t step = elapsedMs / scrollMs;

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
