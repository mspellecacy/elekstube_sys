#include <assert.h>
#include <string.h>
#include "clock_state.h"

int main() {
  LocalClock clock;
  char digits[7];
  assert(!clock.synced());
  assert(!clock.tick(500));
  assert(!clock.sync(LocalClock::kDayMs, 1000));
  assert(!clock.synced());

  assert(clock.sync(29109875, 1000));  // 08:05:09.875, with leading zeros.
  clock.digits(digits);
  assert(strcmp(digits, "080509") == 0);
  assert(!clock.tick(1124));
  assert(clock.tick(1125));
  clock.digits(digits);
  assert(strcmp(digits, "080510") == 0);

  assert(clock.sync(86399875, 2000));
  assert(clock.tick(2125));
  clock.digits(digits);
  assert(strcmp(digits, "000000") == 0);  // Midnight, never 24:00:00.
  assert(clock.tick(622125));  // Continues ticking with no host updates.
  clock.digits(digits);
  assert(strcmp(digits, "001020") == 0);

  assert(clock.sync(86399000, UINT32_MAX - 499));
  assert(clock.tick(500));  // ESP32 millis() wraps after ~49.7 days.
  clock.digits(digits);
  assert(strcmp(digits, "000000") == 0);
  assert(!clock.sync(UINT32_MAX, 600));
  clock.digits(digits);
  assert(strcmp(digits, "000000") == 0);  // Invalid time cannot corrupt clock.

  DebouncedButton button;
  assert(!button.pressed(false, 0));
  assert(!button.pressed(true, 10));
  assert(!button.pressed(false, 15));  // Contact bounce.
  assert(!button.pressed(true, 20));
  assert(!button.pressed(true, 49));
  assert(button.pressed(true, 50));
  assert(!button.pressed(true, 1000));  // Held button never repeats.
  assert(!button.pressed(false, 1010));
  assert(!button.pressed(false, 1040));
  assert(!button.pressed(true, 1050));
  assert(button.pressed(true, 1080));  // Release then another press.

  DebouncedButton wrapButton;
  assert(!wrapButton.pressed(true, UINT32_MAX - 14));
  assert(wrapButton.pressed(true, 15));  // Debounce also survives timer wrap.

  DebouncedButton cycleButton;
  DisplayMode mode = DisplayMode::Monitor;
  const DisplayMode expected[] = {DisplayMode::Nixie,DisplayMode::Flip,DisplayMode::Orbit,DisplayMode::Monitor};
  for (unsigned n = 0; n < 12; ++n) {
    const uint32_t now = 100+n*120;
    assert(!cycleButton.pressed(true,now));
    if (cycleButton.pressed(true,now+30)) mode = nextDisplayMode(mode);
    assert(mode == expected[n%4]);
    assert(isClockMode(mode) == (n%4 != 3));
    assert(!cycleButton.pressed(true,now+70));  // Holding A cannot skip a clock face.
    assert(!cycleButton.pressed(false,now+80));
    assert(!cycleButton.pressed(false,now+110));
  }
  assert(strcmp(displayModeName(DisplayMode::Nixie),"clock") == 0);
  assert(strcmp(displayModeName(DisplayMode::Flip),"flip") == 0);
  assert(strcmp(displayModeName(DisplayMode::Orbit),"orbit") == 0);
}
