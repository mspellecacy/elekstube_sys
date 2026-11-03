// ELEKSTUBE IPS PRO / Gen3 PCB 4.3.3: system monitor and animated clock faces.
// Hardware mapping verified against judge2005/EleksTubeIPS v1.9.5 (V2 target).
#include <Arduino.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <Adafruit_NeoPixel.h>
#include <cmath>
#include "monitor_fonts.h"
#include "clock_state.h"
#include "nixie_renderer.h"
#include "flip_renderer.h"
#include "orbit_renderer.h"
#include "network_link.h"

namespace {
constexpr uint8_t kData = 14, kClock = 9, kLatch = 10, kBacklight = 27;
constexpr uint8_t kScreens = 6;
constexpr uint8_t kButtonA = 33, kButtonD = 34;
constexpr uint32_t kOfflineMs = 6000;
constexpr size_t kMaxLine = 1536;
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
}
constexpr uint16_t kBg = rgb(8, 11, 14), kMuted = rgb(113, 128, 139);
constexpr uint16_t kTrack = rgb(39, 48, 55), kWhite = rgb(238, 243, 246);
constexpr uint16_t kColors[] = {rgb(185, 206, 215), rgb(255, 91, 85), rgb(129, 189, 226),
                                rgb(98, 177, 231), rgb(107, 218, 163), rgb(184, 165, 235)};
constexpr uint8_t kHistory = 60;
TFT_eSPI tft;
TFT_eSprite canvas(&tft);
Adafruit_NeoPixel leds(12, 12, NEO_GRB + NEO_KHZ800);
JsonLines<kMaxLine> usbLines;
NetworkLink network;
bool online = false, screenEnabled = true;
uint32_t usbFrames = 0, tcpFrames = 0;
DisplayMode displayMode = DisplayMode::Monitor;
LocalClock localClock;
NixieRenderer nixie;
FlipRenderer flip;
OrbitRenderer orbit;
// Animation and full redraws share the sprite storage. Every full redraw repaints
// it first, so a second 54 KB transfer buffer would only waste Wi-Fi's memory.
uint16_t *glowTransfer = nullptr;
static_assert(FlipRenderer::kW*FlipRenderer::kH <= 135*240,
              "Clock faces share one transfer buffer");
static_assert(OrbitRenderer::kW*OrbitRenderer::kH <= 135*240,
              "Orbit shares the existing transfer buffer");
DebouncedButton buttonA, buttonD;
uint32_t lastUpdate = 0, frames = 0;
uint32_t animationFrames = 0, animationWorkUs = 0, animationFrameUs = 0;
uint32_t animationStepMs = 0, animationTimeMs = 0;
uint32_t animationRenderUs = 0, animationSpiUs = 0;
uint32_t buttonAPresses = 0;
uint32_t flipUpdates = 0;
uint32_t orbitFrames = 0;
uint8_t animationScreen = 0;
bool orbitDmaReady = false, orbitDmaPending = false;
uint16_t *orbitDmaBuffer = nullptr, *orbitRenderBuffer = nullptr;
uint8_t orbitDmaScreen = 0;
uint8_t orbitBand = 0;
bool orbitDmaLastBand = false;
uint32_t orbitDmaStarted = 0, orbitDmaFrameWorkUs = 0;

void finishOrbitTransfer(bool wait = true);

struct Panel {
  char label[14], value[10], unit[10], detail[20];
  char secondaryValue[10], secondaryUnit[10];
  float percent = -1, secondaryPercent = -1;
};
Panel panels[kScreens];
uint8_t history[kScreens][kHistory] = {};
uint8_t secondaryHistory[kScreens][kHistory] = {};
uint8_t historyCount = 0;

void selectScreens(uint8_t mask) {
  // The shift register is the physical chip select. It must stay on the same
  // tube until DMA has finished, including mode changes, capture and blanking.
  finishOrbitTransfer();
  digitalWrite(kLatch, LOW);
  shiftOut(kData, kClock, LSBFIRST, static_cast<uint8_t>((~mask) << 2));
  digitalWrite(kLatch, HIGH);
}

void finishOrbitTransfer(bool wait) {
  if (!orbitDmaPending || (!wait && tft.dmaBusy())) return;
  tft.dmaWait();
  tft.endWrite();
  animationSpiUs = static_cast<uint32_t>(micros()-orbitDmaStarted);
  orbitDmaPending = false;
  selectScreens(0);
  if (orbitDmaScreen == kScreens-1 && orbitDmaLastBand) {
    ++orbitFrames;  // Count completed six-tube frames, not queued transfers.
    animationFrameUs = orbitDmaFrameWorkUs;
  }
}

void hello(Stream &reply, uint32_t seq = 0) {
  localClock.tick(millis());
  char digits[7];
  localClock.digits(digits);
  reply.printf("{\"type\":\"hello\",\"protocol\":1,\"firmware\":\"ipspro-monitor-1\","
                "\"screens\":6,\"theme\":\"rings\",\"seq\":%lu,\"frames\":%lu,\"online\":%s,\"free_heap\":%u,"
                "\"mode\":\"%s\",\"clock_synced\":%s,\"clock\":\"%s\",\"clock_style\":\"%s\","
                "\"clock_styles\":[\"nixie\",\"flip\",\"orbit\"],\"flip_updates\":%lu,"
                "\"orbit_frames\":%lu,\"orbit_cached\":%s,\"orbit_dma\":%s,\"orbit_bands\":%u,"
                "\"animation_frames\":%lu,\"animation_us\":%lu,\"render_us\":%lu,\"spi_us\":%lu,\"button_a_presses\":%lu,\"animation_cached\":%s,\"usb_frames\":%lu,\"tcp_frames\":%lu,\"wifi\":{\"connected\":%s,\"ip\":\"%s\",\"hostname\":\"elekstube-monitor.local\",\"port\":32100,\"rssi\":%d}}\n",
                static_cast<unsigned long>(seq), static_cast<unsigned long>(frames),
                online ? "true" : "false", ESP.getFreeHeap(), displayModeName(displayMode),
                localClock.synced() ? "true" : "false", localClock.synced() ? digits : "------",
                displayMode == DisplayMode::Orbit ? "orbit" : displayMode == DisplayMode::Flip ? "flip" : "nixie",
                static_cast<unsigned long>(flipUpdates), static_cast<unsigned long>(orbitFrames),
                orbit.cached() ? "true" : "false",
                orbitDmaBuffer ? "true" : "false",
                orbitDmaBuffer ? OrbitRenderer::kBands : 1,
                static_cast<unsigned long>(animationFrames), static_cast<unsigned long>(animationFrameUs),
                static_cast<unsigned long>(animationRenderUs), static_cast<unsigned long>(animationSpiUs),
                static_cast<unsigned long>(buttonAPresses), nixie.cached() ? "true" : "false",
                static_cast<unsigned long>(usbFrames), static_cast<unsigned long>(tcpFrames),
                WiFi.status() == WL_CONNECTED ? "true" : "false",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

void font(const uint8_t *data) {
  static const uint8_t *loaded = nullptr;
  if (loaded != data) { canvas.loadFont(data); loaded = data; }
}

void text(const char *s, int x, int y, uint16_t color = kMuted,
          const uint8_t *face = kLabelFont, uint8_t datum = MC_DATUM) {
  font(face);
  canvas.setTextColor(color, kBg);
  canvas.setTextDatum(datum);
  canvas.drawString(s, x, y);
}

void tracked(const char *s, int y, uint16_t color) {
  font(kLabelFont);
  int width = 0;
  for (const char *c = s; *c; ++c) { char glyph[] = {*c, 0}; width += canvas.textWidth(glyph) + 2; }
  int x = (135 - width + 2) / 2;
  for (const char *c = s; *c; ++c) {
    char glyph[] = {*c, 0};
    text(glyph, x, y, color, kLabelFont, ML_DATUM);
    x += canvas.textWidth(glyph) + 2;
  }
}

void trace(const uint8_t *values, int x, int y, int width, int height,
           uint16_t color, bool fill) {
  if (!online || historyCount < 2) return;
  const int first = kHistory - historyCount;
  float previousX = x + first * width / float(kHistory - 1);
  float previousY = y + height - values[first] * height / 100.0f;
  for (int n = first + 1; n < kHistory; ++n) {
    const float px = x + n * width / float(kHistory - 1);
    const float py = y + height - values[n] * height / 100.0f;
    if (fill) {
      for (int column = lroundf(previousX); column <= lroundf(px); ++column) {
        const float amount = constrain((column - previousX) / (px - previousX), 0.0f, 1.0f);
        const int top = lroundf(previousY + (py - previousY) * amount);
        for (int row = top; row < y + height; ++row) {
          const uint8_t alpha = 10 + (y + height - row) * 35 / height;
          canvas.drawPixel(column, row, canvas.alphaBlend(alpha, color, kBg));
        }
      }
    }
    canvas.drawWideLine(previousX, previousY, px, py, 1.2f, color);
    previousX = px; previousY = py;
  }
  canvas.fillSmoothCircle(previousX, previousY, 2, color);
}

void gauge(uint8_t i) {
  const auto &p = panels[i];
  const uint16_t accent = online ? kColors[i] : kMuted;
  constexpr int cx = 67, cy = 94;
  // The two low-opacity rims soften the ring without a bright bounding box.
  canvas.drawSmoothArc(cx, cy, 59, 58, 0, 360, rgb(17, 24, 29), kBg);
  canvas.drawSmoothArc(cx, cy, 56, 53, 0, 360, kTrack, kBg);
  if (online && p.percent > 0) {
    const auto end = static_cast<uint32_t>(max(1L, lroundf(p.percent * 3.6f)));
    canvas.drawSmoothArc(cx, cy, 58, 52, 0, end, canvas.alphaBlend(28, accent, kBg), kBg, true);
    canvas.drawSmoothArc(cx, cy, 56, 53, 0, end, accent, kBg, true);
  }
  const char *value = online ? p.value : "--";
  font(kValueFont);
  const auto *face = canvas.textWidth(value) > 96 ? kRateFont : kValueFont;
  text(value, cx, 86, online ? kWhite : kMuted, face);
  text(p.unit, cx, 116, accent);
  text(online ? p.detail : "HOST OFFLINE", cx, 164);
  canvas.drawFastHLine(13, 215, 109, rgb(27, 35, 41));
  trace(history[i], 13, 185, 109, 29, canvas.alphaBlend(160, accent, kBg), false);
  text("60s", 13, 230, rgb(76, 91, 102), kLabelFont, ML_DATUM);
  canvas.fillSmoothCircle(118, 230, 2, online ? rgb(111, 177, 147) : kMuted, kBg);
}

void arrow(int x, int y, bool down, uint16_t color) {
  canvas.drawWideLine(x, y - 4, x, y + 4, 1.1f, color, kBg);
  const int tip = down ? y + 4 : y - 4;
  canvas.drawWideLine(x - 3, y, x, tip, 1.1f, color, kBg);
  canvas.drawWideLine(x + 3, y, x, tip, 1.1f, color, kBg);
}

void flowRow(const char *label, const char *value, const char *unit,
             int y, bool down, uint16_t color) {
  arrow(16, y, down, color);
  text(label, 25, y, color, kLabelFont, ML_DATUM);
  text(online ? value : "--", 121, y - 1, online ? kWhite : kMuted, kRateFont, MR_DATUM);
  text(unit, 121, y + 19, kMuted, kLabelFont, MR_DATUM);
}

void traffic(uint8_t i) {
  const auto &p = panels[i];
  const uint16_t primary = online ? kColors[i] : kMuted;
  const uint16_t secondary = online ? rgb(188, 202, 211) : kMuted;
  canvas.drawSmoothRoundRect(10, 42, 7, 6, 114, 91, kTrack, kBg);
  for (int y = 63; y < 127; y += 22) canvas.drawFastHLine(17, y, 101, rgb(19, 27, 33));
  trace(history[i], 17, 51, 101, 75, primary, true);
  trace(secondaryHistory[i], 17, 51, 101, 75, secondary, false);
  text(online ? "60 SECOND HISTORY" : "HOST OFFLINE", 67, 143, rgb(76, 91, 102));
  flowRow(i == 3 ? "RX" : "R", p.value, p.unit, 167, true, primary);
  flowRow(i == 3 ? "TX" : "W", p.secondaryValue, p.secondaryUnit, 213, false, secondary);
}

void prepareClock(uint32_t now) {
  localClock.tick(now);
  char digits[7];
  localClock.digits(digits);
  if (displayMode == DisplayMode::Orbit) orbit.setDigits(digits,localClock.synced(),now);
  else if (displayMode == DisplayMode::Flip) flip.setDigits(digits,localClock.synced(),now);
  else nixie.setDigits(digits, localClock.synced(), now);
}

void drawClock(uint8_t i) {
  if (displayMode == DisplayMode::Orbit) orbit.render(i,millis(),static_cast<uint16_t *>(canvas.getPointer()),true,true);
  else if (displayMode == DisplayMode::Flip) flip.render(i,millis(),static_cast<uint16_t *>(canvas.getPointer()),true,true);
  else nixie.render(i, millis(), static_cast<uint16_t *>(canvas.getPointer()), true, true);
  if (!localClock.synced()) text("WAITING FOR HOST", 67, 220);
}

void drawPanel(uint8_t i) {
  // The sprite may be the DMA source. Drain before any full redraw writes it.
  finishOrbitTransfer();
  canvas.fillSprite(kBg);
  if (isClockMode(displayMode)) drawClock(i);
  else {
    tracked(panels[i].label, 19, online ? rgb(152, 167, 178) : kMuted);
    if (i == 3 || i == 4) traffic(i); else gauge(i);
  }
  // Upstream digit zero is the rightmost physical display.
  selectScreens(1 << (5 - i));
  canvas.pushSprite(0, 0);
}

void drawAll() {
  if (isClockMode(displayMode)) prepareClock(millis());
  for (uint8_t i = 0; i < kScreens; ++i) drawPanel(i);
  selectScreens(0);
}

void pushClockRegion(uint8_t screen, int x, int y, int width, int height, uint32_t started) {
  const uint32_t rendered = micros();
  selectScreens(1 << (5-screen));
  const bool oldSwap = tft.getSwapBytes();
  tft.setSwapBytes(false);
  tft.pushImage(x,y,width,height,glowTransfer);
  tft.setSwapBytes(oldSwap);
  selectScreens(0);
  animationRenderUs = static_cast<uint32_t>(rendered-started);
  animationSpiUs = static_cast<uint32_t>(micros()-rendered);
}

void animateFlip() {
  const uint32_t now = millis();
  prepareClock(now);
  if (static_cast<uint32_t>(now-animationStepMs) < 16) return;
  // Skip settled cards. UART and buttons are polled between every moving card.
  for (uint8_t n = 0; n < kScreens; ++n) {
    const uint8_t screen = animationScreen;
    animationScreen = (animationScreen+1)%kScreens;
    if (!flip.needsFrame(screen,now)) continue;
    animationStepMs = now;
    const uint32_t started = micros();
    flip.renderRegion(screen,now,glowTransfer,true);
    pushClockRegion(screen,FlipRenderer::kX,FlipRenderer::kY,FlipRenderer::kW,FlipRenderer::kH,started);
    ++flipUpdates;
    animationFrameUs = static_cast<uint32_t>(micros()-started);
    return;
  }
}

void animateOrbit() {
  const uint32_t now = millis();
  if (animationScreen == 0 && orbitBand == 0) {
    animationTimeMs = now;
    animationWorkUs = 0;
    prepareClock(now);
  }
  const uint32_t started = micros();
  // Compose into the other buffer while the previous tube is still on the bus.
  orbit.renderBand(animationScreen,animationTimeMs,orbitBand,orbitRenderBuffer,true);
  const uint32_t rendered = micros();
  finishOrbitTransfer();
  selectScreens(1 << (5-animationScreen));
  tft.startWrite();
  orbitDmaStarted = micros();
  // Const overload preserves the renderer's byte order and avoids a copy.
  tft.pushImageDMA(OrbitRenderer::kX,OrbitRenderer::kY+orbitBand*OrbitRenderer::kBandH,
                   OrbitRenderer::kW,OrbitRenderer::kBandH,
                   static_cast<const uint16_t *>(orbitRenderBuffer));
  orbitDmaScreen = animationScreen;
  orbitDmaLastBand = orbitBand == OrbitRenderer::kBands-1;
  orbitDmaPending = true;
  orbitRenderBuffer = orbitRenderBuffer == glowTransfer ? orbitDmaBuffer : glowTransfer;
  animationRenderUs = static_cast<uint32_t>(rendered-started);
  animationWorkUs += static_cast<uint32_t>(micros()-started);
  if (++orbitBand == OrbitRenderer::kBands) {
    orbitBand = 0;
    if (++animationScreen == kScreens) {
      animationScreen = 0;
      orbitDmaFrameWorkUs = animationWorkUs;
    }
  }
}

void animateClock() {
  if (displayMode == DisplayMode::Flip) { animateFlip(); return; }
  if (displayMode == DisplayMode::Orbit && orbitDmaBuffer) { animateOrbit(); return; }
  const uint32_t now = millis();
  // One tube per slice leaves UART processing and button polling between tubes.
  // The minimum slice interval caps the rate when rendering finishes quickly.
  if (static_cast<uint32_t>(now - animationStepMs) < 12) return;
  animationStepMs = now;
  if (animationScreen == 0) {
    animationTimeMs = now;
    animationWorkUs = 0;
    prepareClock(now);
  }
  const uint32_t started = micros();
  if (displayMode == DisplayMode::Orbit) {
    orbit.renderRegion(animationScreen,animationTimeMs,glowTransfer,true);
    pushClockRegion(animationScreen,OrbitRenderer::kX,OrbitRenderer::kY,
                    OrbitRenderer::kW,OrbitRenderer::kH,started);
  } else {
    nixie.renderGlow(animationScreen, animationTimeMs, glowTransfer, true);
    pushClockRegion(animationScreen,NixieRenderer::kGlowX,NixieRenderer::kGlowY,
                    NixieRenderer::kGlowW,NixieRenderer::kGlowH,started);
  }
  animationWorkUs += static_cast<uint32_t>(micros()-started);
  if (++animationScreen == kScreens) {
    animationScreen = 0;
    if (displayMode == DisplayMode::Orbit) ++orbitFrames;
    else ++animationFrames;
    animationFrameUs = animationWorkUs;
  }
}

void setDisplayMode(DisplayMode mode) {
  finishOrbitTransfer();
  if (mode != displayMode) {
    orbitDmaBuffer = orbitRenderBuffer = nullptr;
    nixie.end();
    orbit.end();
    if (mode == DisplayMode::Nixie) { nixie.begin(); nixie.reset(); }
    if (mode == DisplayMode::Flip) flip.reset();
    if (mode == DisplayMode::Orbit) {
      orbit.begin(); orbit.reset();
      if (orbitDmaReady) {
        // Two half-panel buffers fit inside the existing full-screen sprite.
        orbitDmaBuffer = glowTransfer+OrbitRenderer::kW*OrbitRenderer::kBandH;
        orbitRenderBuffer = glowTransfer;
      }
    }
  }
  displayMode = mode;
  animationScreen = 0;
  orbitBand = 0;
  animationStepMs = millis();
  localClock.tick(millis());
  drawAll();
}

void processLine(const char *line, size_t lineLength, Stream &reply, bool tcp) {
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, line);
  if (error) {
    reply.printf("{\"type\":\"error\",\"error\":\"invalid-json\",\"reason\":\"%s\",\"bytes\":%u}\n",
                  error.c_str(), static_cast<unsigned>(lineLength));
    return;
  }
  const char *type = doc["type"] | "";
  if (tcp && !network.authenticated()) {
    if (strcmp(type, "hello") != 0 || !network.authorize(doc["auth"] | "")) {
      reply.println("{\"type\":\"error\",\"error\":\"authentication-required\"}");
      network.reject();
      return;
    }
  }
  if (strcmp(type, "hello") == 0) { hello(reply, doc["seq"] | 0UL); return; }
  if (strcmp(type, "wifi") == 0) {
    if (tcp) { reply.println("{\"type\":\"error\",\"error\":\"usb-only\"}"); return; }
    if (!doc["token"].is<const char *>() ||
        (!doc["ssid"].isNull() && (!doc["ssid"].is<const char *>() || !doc["password"].is<const char *>()))) {
      reply.println("{\"type\":\"error\",\"error\":\"invalid-wifi-settings\"}"); return;
    }
    const bool saved = network.configure(doc["token"].as<const char *>(),
                                        doc["ssid"].as<const char *>(), doc["password"].as<const char *>());
    if (saved) reply.printf("{\"type\":\"wifi\",\"seq\":%lu,\"saved\":true}\n", doc["seq"] | 0UL);
    else reply.println("{\"type\":\"error\",\"error\":\"wifi-settings-not-saved\"}");
    return;
  }
  if (strcmp(type, "mode") == 0) {
    const char *mode = doc["mode"] | "";
    if (strcmp(mode, "clock") != 0 && strcmp(mode, "monitor") != 0 &&
        strcmp(mode,"flip") != 0 && strcmp(mode,"orbit") != 0) {
      reply.println("{\"type\":\"error\",\"error\":\"invalid-mode\"}");
      return;
    }
    setDisplayMode(strcmp(mode,"orbit") == 0 ? DisplayMode::Orbit :
                   strcmp(mode,"flip") == 0 ? DisplayMode::Flip :
                   strcmp(mode,"clock") == 0 ? DisplayMode::Nixie : DisplayMode::Monitor);
    reply.printf("{\"type\":\"mode\",\"seq\":%lu,\"mode\":\"%s\"}\n", doc["seq"] | 0UL, mode);
    return;
  }
  if (strcmp(type, "capture") == 0) {
    if (tcp) { reply.println("{\"type\":\"error\",\"error\":\"usb-only\"}"); return; }
    const int screen = doc["screen"] | -1;
    if (screen < 0 || screen >= kScreens) {
      reply.println("{\"type\":\"error\",\"error\":\"invalid-screen\"}");
      return;
    }
    const uint32_t captureStarted = millis();
    if (isClockMode(displayMode)) prepareClock(captureStarted);
    drawPanel(screen);
    selectScreens(0);
    reply.printf("{\"type\":\"capture\",\"screen\":%d,\"bytes\":64800,\"format\":\"rgb565be\"}\n", screen);
    uint8_t row[270];
    for (int y = 0; y < 240; ++y) {
      for (int x = 0; x < 135; ++x) {
        const uint16_t pixel = canvas.readPixel(x, y);
        row[x * 2] = pixel >> 8; row[x * 2 + 1] = pixel & 255;
      }
      reply.write(row, sizeof(row));
    }
    reply.flush();
    // Capturing blocks the single UART for ~6 seconds. Exclude that deliberate
    // pause from the host timeout so viewing a frame doesn't erase its history.
    if (online) lastUpdate += static_cast<uint32_t>(millis() - captureStarted);
    return;
  }
  if (strcmp(type, "metrics") != 0 || !doc["panels"].is<JsonArray>() || doc["panels"].size() != kScreens) {
    reply.println("{\"type\":\"error\",\"error\":\"expected-six-panels\"}");
    return;
  }
  if (!doc["clock_ms"].isNull() && (!doc["clock_ms"].is<uint32_t>() ||
      doc["clock_ms"].as<uint32_t>() >= LocalClock::kDayMs)) {
    reply.println("{\"type\":\"error\",\"error\":\"invalid-clock\"}");
    return;
  }
  // Validate the entire packet before changing any visible state.
  for (JsonObject p : doc["panels"].as<JsonArray>()) {
    if (!p["label"].is<const char *>() || !p["value"].is<const char *>() ||
        !p["unit"].is<const char *>() || !p["detail"].is<const char *>() ||
        !p["percent"].is<float>() || !std::isfinite(p["percent"].as<float>())) {
      reply.println("{\"type\":\"error\",\"error\":\"invalid-panel\"}");
      return;
    }
    if ((!p["secondary_percent"].isNull() && (!p["secondary_percent"].is<float>() ||
         !std::isfinite(p["secondary_percent"].as<float>()))) ||
        (!p["secondary_value"].isNull() && !p["secondary_value"].is<const char *>()) ||
        (!p["secondary_unit"].isNull() && !p["secondary_unit"].is<const char *>())) {
      reply.println("{\"type\":\"error\",\"error\":\"invalid-secondary\"}");
      return;
    }
  }
  for (uint8_t i = 0; i < kScreens; ++i) {
    JsonObject p = doc["panels"][i];
    strlcpy(panels[i].label, p["label"], sizeof(panels[i].label));
    strlcpy(panels[i].value, p["value"], sizeof(panels[i].value));
    strlcpy(panels[i].unit, p["unit"], sizeof(panels[i].unit));
    strlcpy(panels[i].detail, p["detail"], sizeof(panels[i].detail));
    strlcpy(panels[i].secondaryValue, p["secondary_value"] | "--", sizeof(panels[i].secondaryValue));
    strlcpy(panels[i].secondaryUnit, p["secondary_unit"] | "", sizeof(panels[i].secondaryUnit));
    panels[i].percent = constrain(p["percent"].as<float>(), -1.0f, 100.0f);
    panels[i].secondaryPercent = constrain(p["secondary_percent"] | -1.0f, -1.0f, 100.0f);
    memmove(history[i], history[i] + 1, sizeof(history[i]) - 1);
    memmove(secondaryHistory[i], secondaryHistory[i] + 1, sizeof(secondaryHistory[i]) - 1);
    history[i][kHistory - 1] = static_cast<uint8_t>(max(0L, lroundf(panels[i].percent)));
    secondaryHistory[i][kHistory - 1] = static_cast<uint8_t>(max(0L, lroundf(panels[i].secondaryPercent)));
  }
  if (historyCount < kHistory) ++historyCount;
  online = true;
  lastUpdate = millis();
  const bool wasSynced = localClock.synced();
  if (!doc["clock_ms"].isNull()) localClock.sync(doc["clock_ms"].as<uint32_t>(), lastUpdate);
  if (!isClockMode(displayMode) || (!wasSynced && localClock.synced())) drawAll();
  ++frames;
  if (tcp) ++tcpFrames; else ++usbFrames;
  reply.printf("{\"type\":\"ack\",\"seq\":%lu,\"frames\":%lu}\n",
                doc["seq"].as<unsigned long>(), static_cast<unsigned long>(frames));
}
}  // namespace

void setup() {
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
  // Telemetry is transient; LAN credentials live in an isolated NVS namespace.
  leds.begin();
  leds.clear();
  leds.show();
  pinMode(kData, OUTPUT);
  pinMode(kClock, OUTPUT);
  pinMode(kLatch, OUTPUT);
  pinMode(kBacklight, OUTPUT);
  digitalWrite(kBacklight, LOW);
  // Active-low switches with external pull-up resistors on the V2/Pro board.
  pinMode(kButtonA, INPUT_PULLUP);  // A: monitor -> Nixie -> flip -> Orbit -> monitor.
  pinMode(kButtonD, INPUT);  // D: blank/unblank all screens.
  selectScreens(0x3F);
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(kBg);
  canvas.setColorDepth(16);
  if (!canvas.createSprite(135, 240)) {
    Serial.println("{\"type\":\"error\",\"error\":\"display-buffer-allocation\"}");
    while (true) delay(1000);
  }
  glowTransfer = static_cast<uint16_t *>(canvas.getPointer());
  orbitDmaReady = tft.initDMA(false);
  network.begin();
  const char *labels[] = {"CPU USAGE", "CPU TEMP", "MEMORY", "NETWORK", "DISK I/O", "GPU"};
  for (uint8_t i = 0; i < kScreens; ++i) strlcpy(panels[i].label, labels[i], sizeof(panels[i].label));
  drawAll();
  digitalWrite(kBacklight, HIGH);
  hello(Serial);
}

void loop() {
  finishOrbitTransfer(false);
  // Bounded work per loop keeps disconnect detection and buttons responsive.
  for (unsigned n = 0; n < 512 && Serial.available(); ++n) {
    const char c = Serial.read();
    const auto result = usbLines.push(c);
    if (result == JsonLines<kMaxLine>::Complete) processLine(usbLines.data, usbLines.length, Serial, false);
    else if (result == JsonLines<kMaxLine>::TooLong) Serial.println("{\"type\":\"error\",\"error\":\"line-too-long\"}");
    if (c == '\n') usbLines.reset();
  }
  network.poll(processLine);
  if (online && static_cast<uint32_t>(millis() - lastUpdate) > kOfflineMs) {
    online = false;
    historyCount = 0;
    memset(history, 0, sizeof(history));
    memset(secondaryHistory, 0, sizeof(secondaryHistory));
    if (!isClockMode(displayMode)) drawAll();
  }
  const uint32_t now = millis();
  localClock.tick(now);
  if (buttonA.pressed(digitalRead(kButtonA) == LOW, now)) {
    ++buttonAPresses;
    setDisplayMode(nextDisplayMode(displayMode));
  }
  if (buttonD.pressed(digitalRead(kButtonD) == LOW, now)) {
    screenEnabled = !screenEnabled;
    if (screenEnabled) drawAll();
    selectScreens(0x3F);
    tft.writecommand(screenEnabled ? 0x29 : 0x28);
    selectScreens(0);
    digitalWrite(kBacklight, screenEnabled ? HIGH : LOW);
  }
  if (isClockMode(displayMode) && screenEnabled) animateClock();
  // DMA waits yield while pixels travel. Give the idle task one explicit tick
  // per complete Orbit frame rather than adding a tick to all twelve bands.
  if (displayMode != DisplayMode::Orbit || !orbitDmaBuffer ||
      (animationScreen == 0 && orbitBand == 0) || !screenEnabled) delay(1);
  else delay(0);
}
