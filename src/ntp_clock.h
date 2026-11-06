#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <sys/time.h>
#include "clock_state.h"

// SNTP owns UTC system time; only the main loop touches the display clock.
class NtpClock {
 public:
  void begin() {
    loadSettings();
    ready_ = true;
  }

  bool configure(const char *timezone, const char *server) {
    if (!timezone || !*timezone || strlen(timezone) >= sizeof(timezone_) ||
        !server || !*server || strlen(server) >= sizeof(server_)) return false;
    for (const char *p = timezone; *p; ++p) if (*p < 33 || *p > 126) return false;
    for (const char *p = server; *p; ++p)
      if (!isalnum(static_cast<unsigned char>(*p)) && *p != '.' && *p != '-') return false;
    Preferences prefs;
    if (!prefs.begin("ipsmonitor", false)) return false;
    // Store both values in one NVS entry so a failed write cannot mix settings.
    char settings[sizeof(timezone_) + sizeof(server_)] = {};
    strlcpy(settings, timezone, sizeof(timezone_));
    strlcpy(settings + sizeof(timezone_), server, sizeof(server_));
    bool saved = prefs.putBytes("ntp_settings", settings, sizeof(settings)) == sizeof(settings);
    prefs.end();
    if (!saved) return false;
    strlcpy(timezone_, timezone, sizeof(timezone_));
    strlcpy(server_, server, sizeof(server_));
    started_ = false;
    return true;
  }

 private:
  void loadSettings() {
    Preferences prefs;
    if (!prefs.begin("ipsmonitor", true)) return;
    char settings[sizeof(timezone_) + sizeof(server_)] = {};
    if (prefs.getBytesLength("ntp_settings") == sizeof(settings) &&
        prefs.getBytes("ntp_settings", settings, sizeof(settings)) == sizeof(settings)) {
      settings[sizeof(timezone_) - 1] = '\0';
      settings[sizeof(settings) - 1] = '\0';
      strlcpy(timezone_, settings, sizeof(timezone_));
      strlcpy(server_, settings + sizeof(timezone_), sizeof(server_));
    }
    prefs.end();
  }

 public:
  bool poll(LocalClock &clock, uint32_t now) {
    if (!ready_) return false;
    if (!started_ && WiFi.status() == WL_CONNECTED) {
      configTzTime(timezone_, server_);
      started_ = true;
    }
    if (!started_) return false;
    timeval utc;
    gettimeofday(&utc, nullptr);
    // No host code sets system time: a valid epoch means SNTP has succeeded.
    if (utc.tv_sec < 1704067200) return false;  // 2024-01-01
    tm local;
    if (!localtime_r(&utc.tv_sec, &local)) return false;
    clock.sync((local.tm_hour * 3600UL + local.tm_min * 60UL + local.tm_sec) * 1000UL +
               utc.tv_usec / 1000, now);
    synced_ = true;
    return true;
  }
  bool synced() const { return synced_; }
 private:
  char timezone_[96] = "UTC0", server_[128] = "pool.ntp.org";
  bool ready_ = false, started_ = false, synced_ = false;
};
