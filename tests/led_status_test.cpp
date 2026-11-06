#include <assert.h>
#include "clock_state.h"

int main() {
  DebouncedButton button;

  // A hold fires once after the button has been stable-down for holdMs.
  assert(!button.held(false, 0, 1000));
  assert(!button.held(true, 10, 1000));   // Press edge, not long enough yet.
  assert(!button.held(true, 500, 1000));  // Still holding, below threshold.
  assert(!button.held(true, 1009, 1000)); // Just below holdMs.
  assert(button.held(true, 1010, 1000));  // Hold reported once.
  assert(!button.held(true, 2000, 1000)); // Not repeated while held down.

  // Release resets the hold detector.
  assert(!button.held(false, 2010, 1000));
  assert(!button.held(true, 3000, 1000)); // Re-press below threshold.
  assert(!button.held(true, 3009, 1000));
  assert(button.held(true, 4010, 1000));  // Second hold reported.
  assert(!button.held(false, 4020, 1000));

  // A shorter hold threshold also works.
  DebouncedButton fast;
  assert(!fast.held(true, 10, 50));
  assert(fast.held(true, 60, 50));
  assert(!fast.held(true, 200, 50)); // No repeat.
  assert(!fast.held(false, 210, 50));

  // Timer wrap: a hold started near UINT32_MAX still fires after rollover.
  DebouncedButton wrap;
  assert(!wrap.held(true, UINT32_MAX - 100, 1000));
  assert(wrap.held(true, 900, 1000)); // 1000 ms elapsed across the wrap.
  return 0;
}
