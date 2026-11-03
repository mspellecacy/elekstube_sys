#include <assert.h>
#include <string.h>
#include "timing_tools.h"

int main() {
  char digits[7];
  TimingSession watch;
  watch.digits(digits);
  assert(strcmp(digits,"000000") == 0);
  assert(watch.state() == TimingState::Ready);
  watch.tick(100000);
  assert(watch.milliseconds() == 0);
  watch.press(100000);
  assert(watch.state() == TimingState::Running);
  watch.tick(101234);
  assert(watch.milliseconds() == 1234);
  watch.digits(digits);
  assert(strcmp(digits,"000001") == 0);
  watch.press(102500);
  assert(watch.state() == TimingState::Stopped && watch.milliseconds() == 2500);
  watch.tick(999999);
  assert(watch.milliseconds() == 2500);  // Stopped time does not drift.
  watch.press(1000000);
  assert(watch.state() == TimingState::Ready && watch.milliseconds() == 0);

  TimingSession wrap;
  wrap.press(UINT32_MAX-499);
  wrap.tick(500);
  assert(wrap.milliseconds() == 1000);
  wrap.tick(86400500);
  wrap.digits(digits);
  assert(strcmp(digits,"240001") == 0);  // Elapsed time is not wall-clock time.
  wrap.tick(359999999);
  wrap.tick(360001000);
  assert(wrap.state() == TimingState::Finished);
  wrap.digits(digits);
  assert(strcmp(digits,"995959") == 0);  // Saturate instead of displaying three hour digits.
  wrap.press(360002000);
  assert(wrap.state() == TimingState::Ready && wrap.milliseconds() == 0);

  TimingSession timer(300000);
  timer.digits(digits);
  assert(strcmp(digits,"000500") == 0);
  timer.press(0);
  timer.tick(1);
  timer.digits(digits);
  assert(strcmp(digits,"000500") == 0);
  timer.tick(1000);
  timer.digits(digits);
  assert(strcmp(digits,"000459") == 0);
  timer.press(1500);
  assert(timer.state() == TimingState::Stopped && timer.milliseconds() == 298500);
  timer.tick(1000000);
  assert(timer.milliseconds() == 298500);
  timer.press(1000000);
  assert(timer.state() == TimingState::Ready && timer.milliseconds() == 300000);
  timer.press(UINT32_MAX-499);
  timer.tick(500);
  assert(timer.milliseconds() == 299000);
  assert(timer.tick(300500));  // Completion survives wrap and an overdue tick.
  assert(timer.state() == TimingState::Finished && timer.milliseconds() == 0);
  timer.digits(digits);
  assert(strcmp(digits,"000000") == 0);
  timer.tick(400000);
  assert(timer.milliseconds() == 0);  // Never underflow.
  timer.press(400001);
  assert(timer.state() == TimingState::Ready && timer.milliseconds() == 300000);
  TimingSession longestTimer(UINT32_MAX);
  longestTimer.digits(digits);
  assert(strcmp(digits,"995959") == 0);

  TimingTools tools;
  assert(tools.isClock());
  tools.press(1000);  // C has no effect in normal clock mode.
  tools.next();
  assert(tools.isStopwatch() && tools.session().state() == TimingState::Ready);
  tools.press(1000);
  tools.next();
  assert(tools.session().duration() == 300000);
  tools.press(2000);
  tools.next();
  assert(tools.session().duration() == 600000);
  tools.press(3000);
  tools.tick(4000);
  assert(tools.session().milliseconds() == 599000);
  for (unsigned n = 0; n < TimingTools::kOptions; ++n) tools.next();
  assert(tools.selected() == 3 && tools.session().milliseconds() == 599000);
  for (unsigned n = 0; n < 5; ++n) tools.next();
  assert(tools.isStopwatch() && tools.session().milliseconds() == 3000);
  tools.press(5000);  // Stop just the stopwatch.
  tools.next();
  tools.tick(302000);
  assert(tools.session().state() == TimingState::Finished);
  tools.press(302001);  // Reset this timer without changing the other one.
  tools.next();
  assert(tools.session().state() == TimingState::Running);
  assert(tools.session().milliseconds() == 301000);
  assert(strcmp(timingStateName(TimingState::Finished),"finished") == 0);
}
