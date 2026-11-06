#include <assert.h>
#include <string.h>
#include "timing_tools.h"

int main() {
  char digits[7];

  // setDuration on a stopwatch is a no-op.
  TimingSession watch;
  watch.setDuration(60000);
  assert(watch.duration() == 0);
  assert(watch.milliseconds() == 0);
  assert(watch.state() == TimingState::Ready);
  watch.digits(digits);
  assert(strcmp(digits, "000000") == 0);

  // setDuration arms a timer in the Ready state.
  TimingSession timer(300000);
  timer.setDuration(120000);
  assert(timer.duration() == 120000);
  assert(timer.milliseconds() == 120000);
  assert(timer.state() == TimingState::Ready);
  timer.digits(digits);
  assert(strcmp(digits, "000200") == 0);

  // Duration saturates at kMaximumMs.
  timer.setDuration(UINT32_MAX);
  assert(timer.duration() == TimingSession::kMaximumMs);
  assert(timer.milliseconds() == TimingSession::kMaximumMs);
  timer.digits(digits);
  assert(strcmp(digits, "995959") == 0);

  // Changing duration while running arms it fresh in the Ready state.
  timer.press(1000); // Running.
  assert(timer.state() == TimingState::Running);
  timer.setDuration(5000);
  assert(timer.state() == TimingState::Ready); // Re-armed with new duration.
  assert(timer.duration() == 5000);
  assert(timer.milliseconds() == 5000);
  timer.digits(digits);
  assert(strcmp(digits, "000005") == 0);

  // lastTick_ is cleared so a fresh press starts cleanly.
  timer.press(9000);
  timer.tick(9001);
  assert(timer.milliseconds() == 4999);

  // TimingTools no-ops on clock and stopwatch, applies to timer selections.
  TimingTools tools;
  tools.setSelectedDuration(999999); // Clock selected: no-op.
  assert(!tools.isClock() == false);
  tools.next();                      // Stopwatch.
  tools.setSelectedDuration(42000);
  assert(tools.session().duration() == 0);
  tools.next();                      // First timer preset (5 min).
  assert(tools.session().duration() == 300000);
  tools.setSelectedDuration(123000);
  assert(tools.session().duration() == 123000);
  assert(tools.session().milliseconds() == 123000);
  tools.press(1000);                 // Running now.
  tools.setSelectedDuration(999);    // Re-armed in Ready with new duration.
  assert(tools.session().state() == TimingState::Ready);
  assert(tools.session().duration() == 999);
  tools.next();                      // Select another timer, unchanged preset.
  assert(tools.session().duration() == 600000);
  tools.next();                      // 25 min.
  tools.next();                      // 45 min.
  tools.next();                      // 60 min.
  assert(tools.session().duration() == 3600000);
  tools.setSelectedDuration(1000);
  assert(tools.session().duration() == 1000);
  return 0;
}
