#pragma once
#include <stdint.h>

enum class TimingState : uint8_t { Ready, Running, Stopped, Finished };

inline const char *timingStateName(TimingState state) {
  switch (state) {
    case TimingState::Running: return "running";
    case TimingState::Stopped: return "stopped";
    case TimingState::Finished: return "finished";
    default: return "ready";
  }
}

// Monotonic elapsed time, independent of host clock corrections and timezones.
// A zero duration is a stopwatch; a nonzero duration is a countdown.
class TimingSession {
 public:
  static constexpr uint32_t kMaximumMs = 359999999;  // 99:59:59.999; never wrap at 24 h.
  explicit TimingSession(uint32_t duration = 0)
      : duration_(duration > kMaximumMs ? kMaximumMs : duration), value_(duration_) {}

  bool tick(uint32_t now) {
    if (state_ != TimingState::Running) return false;
    const uint32_t elapsed = now-lastTick_;  // Handles the millis() rollover.
    lastTick_ = now;
    const uint32_t available = duration_ ? value_ : kMaximumMs-value_;
    if (elapsed >= available) {
      value_ = duration_ ? 0 : kMaximumMs;
      state_ = TimingState::Finished;
      return true;
    }
    if (duration_) value_ -= elapsed;
    else value_ += elapsed;
    return false;
  }

  // Deliberately start -> stop -> reset, rather than a pause/resume toggle.
  void press(uint32_t now) {
    tick(now);
    switch (state_) {
      case TimingState::Ready:
        lastTick_ = now;
        state_ = TimingState::Running;
        break;
      case TimingState::Running:
        state_ = TimingState::Stopped;
        break;
      default:
        value_ = duration_;
        state_ = TimingState::Ready;
        break;
    }
  }

  uint32_t milliseconds() const { return value_; }
  uint32_t duration() const { return duration_; }
  TimingState state() const { return state_; }
  void digits(char (&out)[7]) const {
    // A countdown keeps 05:00 visible for its whole first second.
    const uint32_t rounded = (value_+(duration_ ? 999U : 0U))/1000;
    const uint32_t seconds = rounded > kMaximumMs/1000 ? kMaximumMs/1000 : rounded;
    const unsigned parts[] = {seconds/3600, seconds/60%60, seconds%60};
    for (unsigned n = 0; n < 3; ++n) {
      out[n*2] = '0'+parts[n]/10;
      out[n*2+1] = '0'+parts[n]%10;
    }
    out[6] = '\0';
  }

 private:
  uint32_t duration_ = 0, value_ = 0, lastTick_ = 0;
  TimingState state_ = TimingState::Ready;
};

// B selects normal time, stopwatch, or one of five independent timer presets.
// Selecting a different tool never stops, resets or restarts another one.
class TimingTools {
 public:
  static constexpr unsigned kOptions = 7;
  bool isClock() const { return selected_ == 0; }
  bool isStopwatch() const { return selected_ == 1; }
  unsigned selected() const { return selected_; }
  void next() { selected_ = (selected_+1)%kOptions; }
  const char *name() const { return isClock() ? "clock" : isStopwatch() ? "stopwatch" : "timer"; }

  TimingSession &session() { return selected_ < 2 ? stopwatch_ : timers_[selected_-2]; }
  const TimingSession &session() const { return selected_ < 2 ? stopwatch_ : timers_[selected_-2]; }
  void press(uint32_t now) { if (!isClock()) session().press(now); }
  bool tick(uint32_t now) {
    const TimingState before = session().state();
    stopwatch_.tick(now);
    for (auto &timer : timers_) timer.tick(now);
    return !isClock() && before != session().state();
  }

 private:
  unsigned selected_ = 0;
  TimingSession stopwatch_;
  TimingSession timers_[5] = {TimingSession(5*60000), TimingSession(10*60000),
                             TimingSession(25*60000), TimingSession(45*60000),
                             TimingSession(60*60000)};
};
