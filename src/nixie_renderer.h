#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "nixie_assets.h"

#if defined(__GNUC__)
#define NIXIE_OPT __attribute__((optimize("O3")))
#define NIXIE_INLINE inline __attribute__((always_inline))
#else
#define NIXIE_OPT
#define NIXIE_INLINE inline
#endif

// The same renderer runs on ESP32 and desktop, with no allocations per frame.
class NixieRenderer {
 public:
  static constexpr int kWidth = 135, kHeight = 240;
  static constexpr int kX = 0, kY = 22, kW = 135, kH = 202;
  static constexpr int kGlowX = kX, kGlowY = kY, kGlowW = kW, kGlowH = kH;

  // Reserve the display sprite before allocating caches on the device.
  NixieRenderer() { memcpy(basePalette_,kNixieBackgroundPalette,sizeof(basePalette_)); }
  void begin() {
    // Pixel composition should read DRAM, not alternate several flash streams.
    // Small cache blocks fit around the ESP32's separate heap regions.
    background_.allocate(); active_.allocate(); previous_.allocate();
    memcpy(basePalette_,kNixieBackgroundPalette,sizeof(basePalette_));
    background_.load(kNixieBackground+kGlowY*kWidth+kGlowX);
    activeKey_ = previousKey_ = -1;
  }
  NixieRenderer(const NixieRenderer &) = delete;
  NixieRenderer &operator=(const NixieRenderer &) = delete;
  bool cached() const { return background_.ready() && active_.ready() && previous_.ready(); }
  void end() {
    background_.release(); active_.release(); previous_.release();
    activeKey_ = previousKey_ = -1;
  }

  void reset() {
    for (auto &tube : tubes_) tube = Tube{};
  }

  void setDigits(const char *digits, bool synced, uint32_t now) {
    for (unsigned i = 0; i < 6; ++i) {
      const int next = synced && digits[i] >= '0' && digits[i] <= '9' ? digits[i]-'0' : -1;
      auto &tube = tubes_[i];
      if (next != tube.digit) {
        tube.previous = tube.digit;
        tube.digit = next;
        tube.changed = now;
      }
    }
  }

  // Destination is a full panel. `swapped` writes TFT_eSprite's native byte order.
  // Partial frames only touch the glowing region; glass/cage outside it is static.
  NIXIE_OPT void render(unsigned index, uint32_t now, uint16_t *pixels, bool full = true,
                       bool swapped = false, bool compact = false) const {
    if (index >= 6) return;
    if (full) {
      for (int n = 0; n < kWidth*kHeight; ++n) {
        const auto c = basePalette_[byte(kNixieBackground+n)];
        pixels[n] = swapped ? swap(c) : c;
      }
    }
    const auto &tube = tubes_[index];
    const uint32_t age = now - tube.changed;
    // A slow, restrained pulse plus two small unsynchronized supply harmonics.
    const float phase = (now % 228000) * (6.2831853f / 3800.0f) + index*0.29f;
    const float breathing = 0.91f + 0.075f*sinf(phase) +
                            0.009f*sinf(phase*7.0f + index) +
                            0.004f*sinf(phase*19.0f + index*2.0f);
    float incoming = 1.0f, outgoing = 0.0f;
    if (age < 220) {
      // A short overlap, slight ignition dip, then a warm flare that settles.
      const float t = age/220.0f;
      incoming = smooth(t*2.8f);
      if (age >= 55 && age < 110) incoming *= 0.72f;
      incoming *= 1.0f + 0.12f*sinf(t*3.1415927f);
      outgoing = 1.0f-smooth(age/105.0f);
    }
    uint16_t live[256], old[256];
    palette(live, tube.digit < 0 ? 0 : incoming*breathing);
    palette(old, tube.previous < 0 ? 0 : outgoing*breathing*0.8f);
    const bool showOld = tube.previous >= 0 && outgoing > 0;
    // Compose light with each metalwork color once, then the hot pixel loop
    // needs only an indexed lookup. Most frames also swap TFT byte order here.
    for (unsigned base = 0; base < kNixieBackgroundColors; ++base) {
      for (unsigned entry = 0; entry < 256; ++entry) {
        const auto color = add(basePalette_[base],live[entry]);
        composed_[base*256+entry] = swapped && !showOld ? swap(color) : color;
      }
    }
    const auto *currentMap = cache(tube.digit,active_,activeKey_);
    const auto *previousMap = showOld ? cache(tube.previous,previous_,previousKey_) : currentMap;
    for (int y = kGlowY; y < kGlowY+kGlowH; ++y) {
      const int basePosition = (y-kGlowY)*kGlowW;
      const auto *baseRow = background_.row(y-kGlowY,kNixieBackground+kGlowY*kWidth+kGlowX);
      const auto *currentRow = active_.row(y-kY,currentMap);
      const auto *previousRow = previous_.row(y-kY,previousMap);
      int destination = compact ? basePosition : y*kWidth+kGlowX;
      for (int x = 0; x < kGlowW; ++x, ++destination) {
        uint16_t color = composed_[byte(baseRow+x)*256+byte(currentRow+x)];
        if (showOld) {
          color = add(color,old[byte(previousRow+x)]);
          if (swapped) color = swap(color);
        }
        pixels[destination] = color;
      }
    }
  }

  // Contiguous dirty rectangle for one SPI transaction, without a row-by-row copy.
  void renderGlow(unsigned index, uint32_t now, uint16_t *pixels, bool swapped = false) const {
    render(index,now,pixels,false,swapped,true);
  }

 private:
  struct PixelCache {
    static constexpr int kRows = (kH+2)/3;
    uint8_t *parts[3] = {};
    ~PixelCache() { release(); }
    void release() { for (auto &part : parts) { free(part); part = nullptr; } }
    static int length(int part) {
      const int remaining = kH-part*kRows;
      return (remaining < kRows ? remaining : kRows)*kW;
    }
    void allocate() {
      for (int i = 0; i < 3; ++i) {
        if (!parts[i]) parts[i] = static_cast<uint8_t *>(malloc(length(i)));
      }
    }
    bool ready() const { return parts[0] && parts[1] && parts[2]; }
    void load(const uint8_t *source) const {
      for (int i = 0; i < 3; ++i) {
        if (parts[i]) memcpy(parts[i],source+i*kRows*kW,length(i));
      }
    }
    const uint8_t *row(int y, const uint8_t *source) const {
      const auto *part = parts[y/kRows];
      return part ? part+(y%kRows)*kW : source+y*kW;
    }
  };
  struct Tube { int digit = -1, previous = -1; uint32_t changed = 0; };
  Tube tubes_[6];
  uint16_t basePalette_[kNixieBackgroundColors];
  mutable uint16_t composed_[kNixieBackgroundColors*256];
  PixelCache background_, active_, previous_;
  mutable int activeKey_ = -1, previousKey_ = -1;

  static const uint8_t *cache(int digit, const PixelCache &buffer, int &key) {
    if (digit < 0) digit = 0;
    const auto *source = kNixieDistance+digit*kW*kH;
    if (digit != key) {
      buffer.load(source);
      key = digit;
    }
    return source;
  }

  static uint8_t byte(const uint8_t *p) {
#ifdef ARDUINO
    return pgm_read_byte(p);
#else
    return *p;
#endif
  }
  static uint16_t word(const uint16_t *p) {
#ifdef ARDUINO
    return pgm_read_word(p);
#else
    return *p;
#endif
  }
  static uint16_t swap(uint16_t c) { return (c>>8)|(c<<8); }
  static uint32_t minimum(uint32_t a, uint32_t b) { return a < b ? a : b; }
  static float smooth(float t) {
    if (t < 0) return 0;
    if (t > 1) return 1;
    return t*t*(3-2*t);
  }
  static NIXIE_INLINE uint16_t add(uint16_t a, uint16_t b) {
    // Saturated RGB565 addition, with red/blue in separate arithmetic lanes.
    uint32_t rb = (a&0xF81F)+(b&0xF81F), g = (a&0x07E0)+(b&0x07E0);
    const uint32_t rbCarry = rb&0x10020, gCarry = g&0x0800;
    rb |= rbCarry-(rbCarry>>5);
    g |= gCarry-(gCarry>>6);
    return (rb&0xF81F)|(g&0x07E0);
  }
  static NIXIE_OPT void palette(uint16_t *out, float energy) {
    static const unsigned transmission[] = {256,192,88,14};
    for (int mesh = 0; mesh < 4; ++mesh) {
      const auto gain = static_cast<uint32_t>(energy*transmission[mesh]);
      for (int n = 0; n < 64; ++n) {
        const uint32_t r = byte(kNixieEmission+n*3)*gain >> 11;
        const uint32_t g = byte(kNixieEmission+n*3+1)*gain >> 10;
        const uint32_t b = byte(kNixieEmission+n*3+2)*gain >> 11;
        out[n+mesh*64] = (minimum(r,31)<<11)|(minimum(g,63)<<5)|minimum(b,31);
      }
    }
  }
};

#undef NIXIE_OPT
#undef NIXIE_INLINE
