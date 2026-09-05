#include "unity.h"
#include "TextScroll.h"

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
  TEST_ASSERT_EQUAL_UINT8(SCROLL_NONE, scrollModeFromName("nonsense"));
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
  RUN_TEST(test_mode_name_parsing);
  return UNITY_END();
}

int main() { runUnityTests(); }
