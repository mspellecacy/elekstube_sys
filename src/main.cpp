// ELEKSTUBE IPS PRO / Gen3 PCB 4.3.3: system monitor and animated clock faces.
// Hardware mapping verified against judge2005/EleksTubeIPS v1.9.5 (V2 target).
#include <Arduino.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <Adafruit_NeoPixel.h>
#include <cmath>
#include <lwip/apps/sntp.h>
#include "monitor_fonts.h"
#include "clock_state.h"
#include "timing_tools.h"
#include "nixie_renderer.h"
#include "flip_renderer.h"
#include "orbit_renderer.h"
#include "network_link.h"
#include "ntp_clock.h"

namespace {
constexpr uint8_t kData = 14, kClock = 9, kLatch = 10, kBacklight = 27;
constexpr uint8_t kScreens = 6;
constexpr uint8_t kButtonA = 33, kButtonB = 32, kButtonC = 35, kButtonD = 34;
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
DisplayMode lastClockMode = DisplayMode::Nixie;
LocalClock localClock;
NtpClock ntpClock;
TimingTools timingTools;
bool timingLabelsDirty = false;
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
DebouncedButton buttonA, buttonB, buttonC, buttonD;
uint32_t lastUpdate = 0, frames = 0;
uint32_t animationFrames = 0, animationWorkUs = 0, animationFrameUs = 0;
uint32_t animationStepMs = 0, animationTimeMs = 0;
uint32_t animationRenderUs = 0, animationSpiUs = 0;
uint32_t buttonAPresses = 0;
uint32_t buttonBPresses = 0, buttonCPresses = 0;
uint32_t flipUpdates = 0;
uint32_t orbitFrames = 0;
uint8_t animationScreen = 0;
bool orbitDmaReady = false, orbitDmaPending = false;
uint16_t *orbitDmaBuffer = nullptr, *orbitRenderBuffer = nullptr;
uint8_t orbitDmaScreen = 0;
uint8_t orbitBand = 0;
bool orbitDmaLastBand = false;
uint32_t orbitDmaStarted = 0, orbitDmaFrameWorkUs = 0;

// RGB status lighting on the 12 NeoPixels (GPIO 12). Only show() when the color
// changes so idle loops do not churn the bus.
constexpr uint32_t kHoldMs = 1000;      // Long-press threshold shared by all buttons.
constexpr uint8_t kLedCount = 12;
uint32_t lastLedColor = 0;              // Packed 0x00RRGGBB; 0 means off.
uint8_t backlightLevel = 255;           // 100 % → 60 % → 30 % → off → 100 %.

// Read the ESP32's internal die temperature as a standalone fallback when the
// host is offline. Only compiled on the ESP32 target.
float internalCelsius() {
#ifdef temperatureRead
  return temperatureRead();
#else
  return -127.0f;  // Sentinel: not available on this build.
#endif
}

void finishOrbitTransfer(bool wait = true);

void updateTime(uint32_t now) {
  localClock.tick(now);
  ntpClock.poll(localClock, now);
  if (timingTools.tick(now)) timingLabelsDirty = true;
}

bool displayedTimeReady() { return !timingTools.isClock() || localClock.synced(); }

void displayedDigits(char (&out)[7]) {
  if (timingTools.isClock()) localClock.digits(out);
  else timingTools.session().digits(out);
}

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
  updateTime(millis());
  char digits[7], toolDigits[7];
  localClock.digits(digits);
  displayedDigits(toolDigits);
  reply.printf("{\"type\":\"hello\",\"protocol\":1,\"firmware\":\"ipspro-monitor-1\","
                "\"ntp_synced\":%s,\"screens\":6,\"theme\":\"rings\",\"seq\":%lu,\"frames\":%lu,\"online\":%s,\"free_heap\":%u,"
                "\"mode\":\"%s\",\"clock_synced\":%s,\"clock\":\"%s\",\"clock_style\":\"%s\","
                "\"clock_styles\":[\"nixie\",\"flip\",\"orbit\"],\"flip_updates\":%lu,"
                "\"orbit_frames\":%lu,\"orbit_cached\":%s,\"orbit_dma\":%s,\"orbit_bands\":%u,"
                "\"animation_frames\":%lu,\"animation_us\":%lu,\"render_us\":%lu,\"spi_us\":%lu,\"button_a_presses\":%lu,\"animation_cached\":%s,\"usb_frames\":%lu,\"tcp_frames\":%lu,"
                "\"button_b_presses\":%lu,\"button_c_presses\":%lu,\"screen_enabled\":%s,"
                "\"timing\":{\"tool\":\"%s\",\"selection\":%u,\"state\":\"%s\",\"value_ms\":%lu,\"duration_seconds\":%lu,\"digits\":\"%s\"},"
                "\"wifi\":{\"connected\":%s,\"ip\":\"%s\",\"hostname\":\"elekstube-monitor.local\",\"port\":32100,\"rssi\":%d}}\n",
                ntpClock.synced() ? "true" : "false",
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
                static_cast<unsigned long>(buttonBPresses), static_cast<unsigned long>(buttonCPresses),
                screenEnabled ? "true" : "false", timingTools.name(), timingTools.selected(),
                timingTools.isClock() ? "clock" : timingStateName(timingTools.session().state()),
                static_cast<unsigned long>(timingTools.isClock() ? 0 : timingTools.session().milliseconds()),
                static_cast<unsigned long>(timingTools.isClock() ? 0 : timingTools.session().duration()/1000),
                displayedTimeReady() ? toolDigits : "------",
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
  char *value = const_cast<char *>(online ? p.value : "--");
  if (!online && i == 1) {  // CPU TEMP panel: fall back to the ESP32 die sensor.
    const float temp = internalCelsius();
    if (temp > -100.0f) {
      snprintf(panels[i].value, sizeof(panels[i].value), "%.0f", temp);
      value = panels[i].value;
    }
  }
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
  updateTime(now);
  char digits[7];
  displayedDigits(digits);
  if (displayMode == DisplayMode::Orbit) orbit.setDigits(digits,displayedTimeReady(),now);
  else if (displayMode == DisplayMode::Flip) flip.setDigits(digits,displayedTimeReady(),now);
  else nixie.setDigits(digits, displayedTimeReady(), now);
}

void timingLabels(uint8_t screen) {
  if (timingTools.isClock()) return;
  const auto state = timingTools.session().state();
  uint16_t accent = timingTools.isStopwatch() ? rgb(107,218,163) : rgb(255,174,83);
  if (state == TimingState::Finished) accent = rgb(255,91,85);
  else if (state == TimingState::Stopped) accent = kMuted;
  char title[16];
  if (screen < 2) {
    if (timingTools.isStopwatch()) strlcpy(title,"STOPWATCH",sizeof(title));
    else snprintf(title,sizeof(title),"%02u MIN TIMER",static_cast<unsigned>(timingTools.session().duration()/60000));
  } else if (screen < 4) {
    strlcpy(title,state == TimingState::Running ? "RUNNING" :
                  state == TimingState::Stopped ? "STOPPED" :
                  state == TimingState::Finished ? "TIME UP" : "READY",sizeof(title));
  } else {
    strlcpy(title,state == TimingState::Ready ? "C: START" :
                  state == TimingState::Running ? "C: STOP" : "C: RESET",sizeof(title));
  }
  // Both strips are outside every face's animated transfer region.
  canvas.fillRect(0,0,135,21,kBg);
  text(title,67,11,accent);
  canvas.fillRect(0,225,135,15,kBg);
  const char *units[] = {"HOURS","MINUTES","SECONDS"};
  text(units[screen/2],67,232,kMuted);
}

void drawClock(uint8_t i) {
  if (displayMode == DisplayMode::Orbit) orbit.render(i,millis(),static_cast<uint16_t *>(canvas.getPointer()),true,true);
  else if (displayMode == DisplayMode::Flip) flip.render(i,millis(),static_cast<uint16_t *>(canvas.getPointer()),true,true);
  else nixie.render(i, millis(), static_cast<uint16_t *>(canvas.getPointer()), true, true);
  if (timingTools.isClock() && !localClock.synced()) text("WAITING FOR HOST", 67, 220);
  timingLabels(i);
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
  timingLabelsDirty = false;
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
  if (isClockMode(mode)) lastClockMode = mode;
  animationScreen = 0;
  orbitBand = 0;
  animationStepMs = millis();
  updateTime(millis());
  drawAll();
}

// RGB status lighting: one color for the whole strip, refreshed only when it
// changes so the NeoPixel bus stays idle otherwise.
void updateLeds(uint32_t now) {
  uint32_t color = 0;
  if (screenEnabled) {
    if (!timingTools.isClock()) {
      const auto state = timingTools.session().state();
      if (state == TimingState::Running) {
        // Gentle breathing yellow-green while a stopwatch or timer runs.
        const float breathe = 0.5f + 0.5f * sinf(static_cast<float>(now) / 900.0f);
        color = rgb(static_cast<uint8_t>(255 * breathe),
                    static_cast<uint8_t>(180 * breathe), 0);
      } else if (state == TimingState::Finished) {
        color = rgb(255, 0, 0);  // Timer finished: solid red.
      } else {
        color = rgb(64, 64, 0);  // Stopped or ready: dim yellow.
      }
    } else if (online && ntpClock.synced()) {
      color = rgb(0, 255, 0);    // Synced + online: green.
    } else if (WiFi.status() == WL_CONNECTED) {
      color = rgb(0, 0, 255);    // Wi-Fi up, not yet synced: blue.
    } else {
      color = rgb(255, 0, 0);    // Offline: red.
    }
  }
  if (color == lastLedColor) return;
  for (uint8_t n = 0; n < kLedCount; ++n) leds.setPixelColor(n, color);
  leds.show();
  lastLedColor = color;
}

void pressControl(char button) {
  switch (button) {
    case 'A':
      setDisplayMode(nextDisplayMode(displayMode));
      break;
    case 'B':
      timingTools.next();
      setDisplayMode(isClockMode(displayMode) ? displayMode : lastClockMode);
      break;
    case 'C':
      if (!timingTools.isClock()) {
        timingTools.press(millis());
        setDisplayMode(isClockMode(displayMode) ? displayMode : lastClockMode);
      }
      break;
    case 'D':
      screenEnabled = !screenEnabled;
      if (screenEnabled) drawAll();
      selectScreens(0x3F);
      tft.writecommand(screenEnabled ? 0x29 : 0x28);
      selectScreens(0);
      digitalWrite(kBacklight, screenEnabled ? HIGH : LOW);
      break;
  }
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
  if (strcmp(type, "control") == 0) {
    const char *button = doc["button"] | "";
    if (strlen(button) != 1 || button[0] < 'A' || button[0] > 'D') {
      reply.println("{\"type\":\"error\",\"error\":\"invalid-control\"}"); return;
    }
    pressControl(button[0]);
    reply.printf("{\"type\":\"control\",\"seq\":%lu,\"button\":\"%c\"}\n",doc["seq"] | 0UL,button[0]);
    return;
  }
  if (strcmp(type, "ntp-status") == 0) {
    JsonDocument status;
    status["type"] = "ntp-status";
    status["seq"] = doc["seq"] | 0UL;
    status["enabled"] = bool(sntp_enabled());
    status["synced"] = ntpClock.synced();
    status["epoch"] = static_cast<long>(time(nullptr));
    status["server"] = sntp_getservername(0);
    status["server_ip"] = IPAddress(ip_2_ip4(sntp_getserver(0))->addr).toString();
    status["gateway"] = WiFi.gatewayIP().toString();
    status["dns"] = WiFi.dnsIP().toString();
    status["uptime_ms"] = millis();
    serializeJson(status, reply);
    reply.println();
    return;
  }
  if (strcmp(type, "ntp") == 0) {
    if (!doc["timezone"].is<const char *>() || !doc["server"].is<const char *>() ||
        !ntpClock.configure(doc["timezone"].as<const char *>(), doc["server"].as<const char *>())) {
      reply.println("{\"type\":\"error\",\"error\":\"invalid-or-unsaved-ntp-settings\"}");
      return;
    }
    reply.printf("{\"type\":\"ntp\",\"seq\":%lu,\"saved\":true}\n", doc["seq"] | 0UL);
    return;
  }
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
  if (strcmp(type, "timer") == 0) {
    // Set the selected countdown's duration (1–99 minutes). No effect on clock
    // or stopwatch; stopwatches keep a fixed zero duration.
    if (timingTools.isClock() || timingTools.isStopwatch()) {
      reply.println("{\"type\":\"error\",\"error\":\"invalid-tool\"}");
      return;
    }
    const auto &durationField = doc["duration_seconds"];
    if (!durationField.is<unsigned int>() ||
        durationField.as<unsigned int>() < 1 ||
        durationField.as<unsigned int>() > 5940) {
      reply.println("{\"type\":\"error\",\"error\":\"invalid-timer-duration\"}");
      return;
    }
    const uint32_t seconds = durationField.as<unsigned int>();
    timingTools.setSelectedDuration(seconds * 1000);
    reply.printf("{\"type\":\"timer\",\"seq\":%lu,\"duration_seconds\":%u,\"saved\":true}\n",
                 doc["seq"] | 0UL, seconds);
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
  if (!ntpClock.synced() && !doc["clock_ms"].isNull()) localClock.sync(doc["clock_ms"].as<uint32_t>(), lastUpdate);
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
  pinMode(kButtonB, INPUT_PULLUP);  // B: time -> stopwatch -> countdown presets.
  pinMode(kButtonC, INPUT);         // C: start -> stop -> reset; external pull-up.
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
  ntpClock.begin();
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
  updateTime(now);
  updateLeds(now);
  if (buttonA.pressed(digitalRead(kButtonA) == LOW, now)) {
    ++buttonAPresses;
    pressControl('A');
  }
  if (buttonA.held(digitalRead(kButtonA) == LOW, now, kHoldMs)) {
    setDisplayMode(DisplayMode::Monitor);
  }
  if (buttonB.pressed(digitalRead(kButtonB) == LOW, now)) {
    ++buttonBPresses;
    pressControl('B');
  }
  if (buttonC.pressed(digitalRead(kButtonC) == LOW, now)) {
    ++buttonCPresses;
    pressControl('C');
  }
  if (buttonD.pressed(digitalRead(kButtonD) == LOW, now)) {
    pressControl('D');
  }
  if (buttonD.held(digitalRead(kButtonD) == LOW, now, kHoldMs)) {
    // Cycle backlight brightness: 100 → 60 → 30 → off → 100.
    static constexpr uint8_t kLevels[] = {255, 153, 76, 0};
    uint8_t next = kLevels[0];
    for (unsigned n = 0; n < sizeof(kLevels); ++n) {
      if (kLevels[n] == backlightLevel) { next = kLevels[(n + 1) % sizeof(kLevels)]; break; }
    }
    backlightLevel = next;
    analogWrite(kBacklight, backlightLevel);
  }
  if (timingLabelsDirty && isClockMode(displayMode)) drawAll();
  if (isClockMode(displayMode) && screenEnabled) animateClock();
  // DMA waits yield while pixels travel. Give the idle task one explicit tick
  // per complete Orbit frame rather than adding a tick to all twelve bands.
  if (displayMode != DisplayMode::Orbit || !orbitDmaBuffer ||
      (animationScreen == 0 && orbitBand == 0) || !screenEnabled) delay(1);
  else delay(0);
}
