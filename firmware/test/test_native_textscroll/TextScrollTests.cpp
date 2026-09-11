#include "unity.h"
#include <stdio.h>
#include "TextScroll.h"
#include "ScrollVectors.h"

// Lives in its own suite directory rather than alongside test_native/SimpleTests.cpp:
// PlatformIO compiles every .cpp in a suite dir into a single binary, and
// SimpleTests.cpp already defines setUp/tearDown/runUnityTests/main, so sharing a
// directory is a duplicate-symbol link error, not a bigger test run.

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
  TEST_ASSERT_EQUAL_INT16(0,   textScrollOffset(100, 62, 76 * 40, 40, SCROLL_LOOP).offsetX);
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

// Mirrors Clockface::scrollModeFromName. Kept in the pure library so the
// mapping from document string to enum is tested without a panel.
void test_mode_name_parsing(void) {
  TEST_ASSERT_EQUAL_UINT8(SCROLL_NONE, scrollModeFromName(nullptr));
  TEST_ASSERT_EQUAL_UINT8(SCROLL_NONE, scrollModeFromName(""));
  TEST_ASSERT_EQUAL_UINT8(SCROLL_NONE, scrollModeFromName("none"));
  TEST_ASSERT_EQUAL_UINT8(SCROLL_ONCE, scrollModeFromName("once"));
  TEST_ASSERT_EQUAL_UINT8(SCROLL_LOOP, scrollModeFromName("loop"));
  TEST_ASSERT_EQUAL_UINT8(SCROLL_TICKER, scrollModeFromName("ticker"));
  TEST_ASSERT_EQUAL_UINT8(SCROLL_NONE, scrollModeFromName("nonsense"));
}

// The ticker (clockwise#18). SCROLL_LOOP is a ping-pong and SCROLL_ONCE parks at
// the head, so before this the panel had no way to show a long line in full more
// than once. These pin the two properties that make it a ticker rather than
// either of those: it cycles over the PITCH, and it never finishes.
void test_ticker_cycles_over_pitch_not_travel(void) {
  const uint16_t textW = 100, boxW = 62;
  const uint16_t pitch = scrollTickerPitch(textW);   // 108
  TEST_ASSERT_EQUAL_UINT16(textW + SCROLL_TICKER_GAP, pitch);

  // At the step where travel runs out a ping-pong turns around. A ticker does
  // not: the tail has left the box and the repeat is still coming in.
  const uint16_t travel = textW - boxW;             // 38
  ScrollResult r = textScrollOffset(textW, boxW, travel * 40UL, 40, SCROLL_TICKER);
  TEST_ASSERT_EQUAL_INT16(-(int16_t)travel, r.offsetX);
  TEST_ASSERT_FALSE(r.done);

  r = textScrollOffset(textW, boxW, (travel + 1) * 40UL, 40, SCROLL_TICKER);
  TEST_ASSERT_EQUAL_INT16(-(int16_t)(travel + 1), r.offsetX);
}

void test_ticker_wrap_is_seamless(void) {
  const uint16_t textW = 100, boxW = 62;
  const uint16_t pitch = scrollTickerPitch(textW);

  // The frame one short of the pitch is the furthest left it ever goes.
  ScrollResult last = textScrollOffset(textW, boxW, (pitch - 1) * 40UL, 40, SCROLL_TICKER);
  TEST_ASSERT_EQUAL_INT16(-(int16_t)(pitch - 1), last.offsetX);

  // And at exactly the pitch it is back at zero, which is the SAME PICTURE:
  // drawTextBoxed() draws the string again one pitch to the right, so the copy
  // that was at +pitch has arrived where the first copy started. Nothing jumps.
  ScrollResult wrapped = textScrollOffset(textW, boxW, pitch * 40UL, 40, SCROLL_TICKER);
  TEST_ASSERT_EQUAL_INT16(0, wrapped.offsetX);
  TEST_ASSERT_FALSE(wrapped.done);
}

void test_ticker_never_finishes(void) {
  // SCROLL_ONCE reports done after travel + SCROLL_HOLD_STEPS and stops being
  // redrawn, which is what parked a long line at its own first 62px forever.
  ScrollResult once = textScrollOffset(100, 62, 100000UL, 40, SCROLL_ONCE);
  TEST_ASSERT_TRUE(once.done);
  TEST_ASSERT_EQUAL_INT16(0, once.offsetX);

  // A ticker at the same elapsed time is still moving, and still not done, at
  // every lap.
  for (uint32_t el = 0; el < 500000UL; el += 37000UL) {
    ScrollResult r = textScrollOffset(100, 62, el, 40, SCROLL_TICKER);
    TEST_ASSERT_FALSE(r.done);
    TEST_ASSERT_TRUE(r.offsetX <= 0);
    TEST_ASSERT_TRUE(r.offsetX > -(int16_t)scrollTickerPitch(100));
  }
}

void test_ticker_that_fits_never_moves(void) {
  // A line inside its box is not truncated, so there is nothing to scroll and
  // moving it would be motion for its own sake.
  ScrollResult r = textScrollOffset(40, 62, 100000UL, 40, SCROLL_TICKER);
  TEST_ASSERT_EQUAL_INT16(0, r.offsetX);
  TEST_ASSERT_TRUE(r.done);
}

// Shared with the JS renderer in the panel-canvas repo. These rows come from
// one file (test-vectors/scroll.tsv) so the studio's preview cannot drift from
// the panel without failing a test in both languages. This file is the
// reference: it is the code that actually runs on the wall.
void test_shared_vectors(void) {
  for (unsigned i = 0; i < SCROLL_VECTOR_COUNT; i++) {
    const ScrollVector &v = SCROLL_VECTORS[i];
    ScrollResult r = textScrollOffset(v.textW, v.boxW, v.elapsedMs, v.scrollMs, v.mode);
    char msg[96];
    snprintf(msg, sizeof(msg), "vector %u (tw=%u bw=%u el=%lu mode=%u)",
             i, v.textW, v.boxW, (unsigned long)v.elapsedMs, v.mode);
    TEST_ASSERT_EQUAL_INT16_MESSAGE(v.offsetX, r.offsetX, msg);
    TEST_ASSERT_EQUAL_MESSAGE(v.done, r.done, msg);
  }
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
  RUN_TEST(test_ticker_cycles_over_pitch_not_travel);
  RUN_TEST(test_ticker_wrap_is_seamless);
  RUN_TEST(test_ticker_never_finishes);
  RUN_TEST(test_ticker_that_fits_never_moves);
  RUN_TEST(test_zero_scrollms_is_treated_as_one);
  RUN_TEST(test_zero_box_width_does_not_crash);
  RUN_TEST(test_zero_text_width_fits);
  RUN_TEST(test_mode_name_parsing);
  RUN_TEST(test_shared_vectors);
  return UNITY_END();
}

int main() { runUnityTests(); }
