#pragma once
#include <stdint.h>

enum class DisplayMode : uint8_t { Monitor, Nixie, Flip, Orbit };
inline bool isClockMode(DisplayMode mode) { return mode != DisplayMode::Monitor; }
inline DisplayMode nextDisplayMode(DisplayMode mode) {
  switch (mode) {
    case DisplayMode::Monitor: return DisplayMode::Nixie;
    case DisplayMode::Nixie: return DisplayMode::Flip;
    case DisplayMode::Flip: return DisplayMode::Orbit;
    default: return DisplayMode::Monitor;
  }
}
inline const char *displayModeName(DisplayMode mode) {
  switch (mode) {
    case DisplayMode::Nixie: return "clock";  // Existing USB command stays compatible.
    case DisplayMode::Flip: return "flip";
    case DisplayMode::Orbit: return "orbit";
    default: return "monitor";
  }
}

// Local milliseconds since midnight, supplied by the host or NTP adapter.
class LocalClock {
 public:
  static constexpr uint32_t kDayMs = 86400000;

  bool sync(uint32_t milliseconds, uint32_t now) {
    if (milliseconds >= kDayMs) return false;
    milliseconds_ = milliseconds;
    lastTick_ = now;
    synced_ = true;
    return true;
  }

  bool tick(uint32_t now) {
    if (!synced_) return false;
    const uint32_t previousSecond = milliseconds_ / 1000;
    const uint32_t elapsed = now - lastTick_;  // Also handles millis() rollover.
    milliseconds_ = (milliseconds_ + elapsed % kDayMs) % kDayMs;
    lastTick_ = now;
    return milliseconds_ / 1000 != previousSecond;
  }

  bool synced() const { return synced_; }

  void digits(char (&out)[7]) const {
    const uint32_t seconds = milliseconds_ / 1000;
    const uint8_t parts[] = {static_cast<uint8_t>(seconds / 3600),
                             static_cast<uint8_t>(seconds / 60 % 60),
                             static_cast<uint8_t>(seconds % 60)};
    for (uint8_t pair = 0; pair < 3; ++pair) {
      out[pair * 2] = '0' + parts[pair] / 10;
      out[pair * 2 + 1] = '0' + parts[pair] % 10;
    }
    out[6] = '\0';
  }

 private:
  uint32_t milliseconds_ = 0, lastTick_ = 0;
  bool synced_ = false;
};

// A press fires once after a stable 30 ms; holding cannot repeat it.
class DebouncedButton {
 public:
  bool pressed(bool down, uint32_t now) {
    if (down != rawDown_) { rawDown_ = down; changed_ = now; }
    if (down != stableDown_ && static_cast<uint32_t>(now - changed_) >= 30) {
      stableDown_ = down;
      return down;
    }
    return false;
  }

  // A hold fires once after the button has been stable-down for holdMs; it does
  // not repeat while the button stays down and resets on release. Query both
  // pressed() and held() each loop with the same reading; they track independent
  // edge state so neither corrupts the other.
  bool held(bool down, uint32_t now, uint32_t holdMs) {
    if (down != rawDown_) { rawDown_ = down; changed_ = now; }
    if (rawDown_ && !holdStable_ &&
        static_cast<uint32_t>(now - changed_) >= 30) {
      holdStable_ = true;
    }
    if (!rawDown_) { longReported_ = false; holdStable_ = false; return false; }
    if (!longReported_ && holdStable_ &&
        static_cast<uint32_t>(now - changed_) >= holdMs) {
      longReported_ = true;
      return true;
    }
    return false;
  }

 private:
  bool rawDown_ = false, stableDown_ = false, holdStable_ = false,
      longReported_ = false;
  uint32_t changed_ = 0;
};
