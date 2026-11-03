#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "json_lines.h"

// One authenticated LAN client. Provisioning remains USB-only; the original
// firmware's settings are never overwritten. No HTTP server or frame streaming.
class NetworkLink {
 public:
  static constexpr uint16_t kPort = 32100;
  static constexpr const char *kHostname = "elekstube-monitor";
  void begin() {
    Preferences prefs;
    if (prefs.begin("ipsmonitor", true)) {
      prefs.getString("token", token_, sizeof(token_));
      prefs.getString("ssid", ssid_, sizeof(ssid_));
      prefs.getString("password", password_, sizeof(password_));
      prefs.end();
    }
    WiFi.persistent(false);  // Reuse factory station credentials, keep changes in RAM.
    WiFi.setHostname(kHostname);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    connect();
  }

  bool configure(const char *token, const char *ssid, const char *password) {
    if (!validToken(token) || (ssid && (!*ssid || strlen(ssid) > 32 ||
        !password || strlen(password) > 64 || (strlen(password) > 0 && strlen(password) < 8)))) return false;
    Preferences prefs;
    if (!prefs.begin("ipsmonitor", false)) return false;
    bool saved = prefs.putString("token", token) == strlen(token);
    if (ssid) {
      saved = prefs.putString("ssid", ssid) == strlen(ssid) && saved;
      saved = prefs.putString("password", password) == strlen(password) && saved;
    }
    prefs.end();
    if (!saved) return false;
    strlcpy(token_, token, sizeof(token_));
    if (ssid) {
      strlcpy(ssid_, ssid, sizeof(ssid_));
      strlcpy(password_, password, sizeof(password_));
    }
    reconnect_ = true;
    client_.stop();
    authenticated_ = false;
    return true;
  }

  bool authorize(const char *token) {
    if (!validToken(token) || !validToken(token_)) return false;
    unsigned difference = 0;
    for (unsigned n = 0; n < 64; ++n) difference |= token[n] ^ token_[n];
    authenticated_ = difference == 0;
    return authenticated_;
  }
  bool authenticated() const { return authenticated_; }
  void reject() { client_.stop(); authenticated_ = false; }

  template <typename Handler> void poll(Handler handler) {
    if (reconnect_) {
      reconnect_ = false;
      WiFi.disconnect(false, false);
      connect();
    }
    const uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) {
      if (listening_) {
        reject(); server_.end(); MDNS.end(); listening_ = false;
      }
      if (static_cast<uint32_t>(now-lastConnect_) > 15000) connect();
      return;
    }
    if (!listening_) {
      if (MDNS.begin(kHostname)) MDNS.addService("elekstube", "tcp", kPort);
      server_.begin();
      server_.setNoDelay(true);
      listening_ = true;
    }
    if (client_ && static_cast<uint32_t>(now-lastInput_) > (authenticated_ ? 10000U : 2000U)) reject();
    if (server_.hasClient()) {
      WiFiClient incoming = server_.available();
      if (!client_ || !client_.connected()) {
        client_ = incoming;
        client_.setNoDelay(true);
        client_.setTimeout(1);
        authenticated_ = false;
        lines_.reset();
        lastInput_ = now;
      } else incoming.stop();
    }
    for (unsigned n = 0; n < 512 && client_.available(); ++n) {
      const char c = client_.read();
      lastInput_ = now;
      const auto result = lines_.push(c);
      if (result == JsonLines<1536>::Complete) handler(lines_.data, lines_.length, client_, true);
      else if (result == JsonLines<1536>::TooLong) client_.println("{\"type\":\"error\",\"error\":\"line-too-long\"}");
      if (c == '\n') lines_.reset();
    }
  }

 private:
  static bool validToken(const char *token) {
    if (!token || strlen(token) != 64) return false;
    for (unsigned n = 0; n < 64; ++n) if (!isxdigit(static_cast<unsigned char>(token[n]))) return false;
    return true;
  }
  void connect() {
    lastConnect_ = millis();
    if (*ssid_) WiFi.begin(ssid_, password_);
    else WiFi.begin();
  }
  WiFiServer server_{kPort, 1};
  WiFiClient client_;
  JsonLines<1536> lines_;
  char token_[65] = {}, ssid_[33] = {}, password_[65] = {};
  bool listening_ = false, authenticated_ = false, reconnect_ = false;
  uint32_t lastInput_ = 0, lastConnect_ = 0;
};
