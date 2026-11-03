#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "flip_assets.h"

#if defined(__GNUC__)
#define FLIP_OPT __attribute__((optimize("O3")))
#else
#define FLIP_OPT
#endif

// Shared by the device and desktop preview. No heap or per-frame allocations.
class FlipRenderer {
 public:
  static constexpr int kWidth = 135, kHeight = 240;
  static constexpr int kX = 6, kY = 26, kW = 123, kH = 190;
  static constexpr int kGlyphX = 19, kGlyphY = 43, kGlyphW = 97, kGlyphH = 154;
  static constexpr int kGlyphBytes = (kGlyphW*kGlyphH+1)/2;
  static constexpr uint32_t kDuration = 520;

  FlipRenderer() {
    memcpy(base_,kFlipPalette,sizeof(base_));
    for (unsigned color = 0; color < kFlipColors; ++color) {
      for (unsigned alpha = 0; alpha < 16; ++alpha) {
        normal_[color*16+alpha] = blend(base_[color],rgb(239,235,221),alpha);
      }
    }
  }
  void reset() { for (auto &tube : tubes_) tube = Tube{}; }
  void setDigits(const char *digits, bool synced, uint32_t now) {
    for (unsigned i = 0; i < 6; ++i) {
      const int target = synced && digits[i] >= '0' && digits[i] <= '9' ? digits[i]-'0' : -1;
      auto &tube = tubes_[i];
      if (target != tube.digit) {
        tube.previous = tube.digit;
        tube.digit = target;
        tube.changed = now;
        tube.settled = false;
      }
    }
  }
  bool needsFrame(unsigned index, uint32_t now) const {
    return index < 6 && (!tubes_[index].settled || moving(tubes_[index],now));
  }

  FLIP_OPT void render(unsigned index, uint32_t now, uint16_t *pixels,
                       bool full = true, bool swapped = false, bool compact = false) {
    if (index >= 6) return;
    auto &tube = tubes_[index];
    const auto *current = glyph(tube.digit,current_,currentKey_);
    const bool inMotion = moving(tube,now);
    const auto *previous = inMotion ? glyph(tube.previous,previous_,previousKey_) : current;
    const int startY = full ? 0 : kY, endY = full ? kHeight : kY+kH;
    const int startX = full ? 0 : kX, endX = full ? kWidth : kX+kW;
    for (int y = startY; y < endY; ++y) {
      int dest = position(startX,y,compact);
      for (int x = startX; x < endX; ++x, ++dest) pixels[dest] = background(x,y);
    }
    // The upper incoming card is revealed while the outgoing upper leaf folds.
    drawHalf(current,true,pixels,compact);
    drawHalf(inMotion ? previous : current,false,pixels,compact);
    if (inMotion) {
      const uint32_t age = now-tube.changed;
      const bool top = age < 200;
      float scale;
      if (age < 400) scale = fabsf(cosf(age*(3.14159265f/400.0f)));
      else {
        const float t = (age-400)/120.0f;
        scale = 1.0f-0.027f*sinf(t*3.14159265f)*expf(-2.0f*t);
      }
      const int height = static_cast<int>(90*scale+0.5f);
      const int edge = top ? 120-height : 120+height;
      if (top) {
        shadow(pixels,compact,edge-6,edge,static_cast<unsigned>((1-scale)*85));
        shadow(pixels,compact,122,133,static_cast<unsigned>((1-scale)*100));
      } else shadow(pixels,compact,edge,edge+10,static_cast<unsigned>((1-scale)*130));
      if (height > 0) leaf(top ? previous : current,top,height,scale,pixels,compact);
    }
    // The seam and fixed hinge hardware always sit in front of the moving leaf.
    restore(pixels,compact,9,118,126,122);
    restore(pixels,compact,6,114,13,127);
    restore(pixels,compact,122,114,129,127);
    if (swapped) {
      for (int y = startY; y < endY; ++y) {
        int dest = position(startX,y,compact);
        for (int x = startX; x < endX; ++x, ++dest) pixels[dest] = swap(pixels[dest]);
      }
    }
    tube.settled = !inMotion;
  }
  void renderRegion(unsigned index, uint32_t now, uint16_t *pixels, bool swapped = false) {
    render(index,now,pixels,false,swapped,true);
  }

 private:
  struct Tube { int digit = -1, previous = -1; uint32_t changed = 0; bool settled = false; };
  Tube tubes_[6];
  uint16_t base_[kFlipColors], normal_[kFlipColors*16];
  uint8_t current_[kGlyphBytes], previous_[kGlyphBytes];
  int currentKey_ = -1, previousKey_ = -1;

  static bool moving(const Tube &tube, uint32_t now) {
    return tube.digit >= 0 && tube.previous >= 0 && now-tube.changed < kDuration;
  }
  static const uint8_t *glyph(int digit, uint8_t *cache, int &key) {
    if (digit < 0) return nullptr;
    if (digit != key) {
      memcpy(cache,kFlipGlyphs+digit*kGlyphBytes,kGlyphBytes);
      key = digit;
    }
    return cache;
  }
  static unsigned alpha(const uint8_t *glyph, int x, int y) {
    if (!glyph || x < kGlyphX || x >= kGlyphX+kGlyphW || y < kGlyphY || y >= kGlyphY+kGlyphH) return 0;
    const unsigned n = (y-kGlyphY)*kGlyphW+x-kGlyphX;
    return (glyph[n/2] >> (n%2 ? 0 : 4)) & 15;
  }
  static uint8_t byte(const uint8_t *address) {
#ifdef ARDUINO
    return pgm_read_byte(address);
#else
    return *address;
#endif
  }
  static uint16_t rgb(unsigned r, unsigned g, unsigned b) {
    return ((r&248)<<8)|((g&252)<<3)|(b>>3);
  }
  static uint16_t swap(uint16_t value) { return (value>>8)|(value<<8); }
  static uint16_t blend(uint16_t bg, uint16_t fg, unsigned alpha) {
    const unsigned inverse = 15-alpha;
    const unsigned r = (((bg>>11)*inverse+(fg>>11)*alpha+7)/15);
    const unsigned g = ((((bg>>5)&63)*inverse+((fg>>5)&63)*alpha+7)/15);
    const unsigned b = (((bg&31)*inverse+(fg&31)*alpha+7)/15);
    return (r<<11)|(g<<5)|b;
  }
  static uint16_t shade(uint16_t color, unsigned gain) {
    return (((color>>11)*gain>>8)<<11)|(((((color>>5)&63)*gain)>>8)<<5)|(((color&31)*gain)>>8);
  }
  static int position(int x, int y, bool compact) {
    return compact ? (y-kY)*kW+x-kX : y*kWidth+x;
  }
  uint16_t background(int x, int y) const { return base_[byte(kFlipBackground+y*kWidth+x)]; }
  void drawHalf(const uint8_t *glyph, bool top, uint16_t *pixels, bool compact) const {
    if (!glyph) return;
    const int start = top ? kGlyphY : 122, end = top ? 118 : kGlyphY+kGlyphH;
    for (int y = start; y < end; ++y) {
      int dest = position(kGlyphX,y,compact);
      for (int x = kGlyphX; x < kGlyphX+kGlyphW; ++x, ++dest) {
        const auto a = alpha(glyph,x,y);
        if (a) pixels[dest] = normal_[byte(kFlipBackground+y*kWidth+x)*16+a];
      }
    }
  }
  FLIP_OPT void leaf(const uint8_t *glyph, bool top, int height, float scale,
                    uint16_t *pixels, bool compact) const {
    const unsigned gain = static_cast<unsigned>(105+151*scale);
    const auto face = shade(top ? rgb(32,36,43) : rgb(27,30,36),gain);
    const auto white = shade(rgb(239,235,221),gain);
    const auto edge = shade(rgb(72,76,83),gain);
    uint16_t palette[16];
    for (unsigned n = 0; n < 16; ++n) palette[n] = blend(face,white,n);
    const int start = top ? 120-height : 120, end = top ? 120 : 120+height;
    for (int y = start; y < end; ++y) {
      const int sourceY = 120+(y-120)*90/height;
      const int freeRow = top ? y-start : end-1-y;
      const int inset = freeRow == 0 ? 4 : freeRow == 1 ? 2 : 0;
      int dest = position(9+inset,y,compact);
      for (int x = 9+inset; x < 126-inset; ++x, ++dest) {
        pixels[dest] = freeRow == 0 ? edge : palette[alpha(glyph,x,sourceY)];
      }
    }
  }
  static void shadow(uint16_t *pixels, bool compact, int start, int end, unsigned darkness) {
    for (int y = start; y < end; ++y) {
      if (y < 31 || y >= 209 || (y >= 118 && y < 122)) continue;
      const unsigned gain = 256-darkness*(end-y)/(end-start);
      int dest = position(10,y,compact);
      for (int x = 10; x < 125; ++x, ++dest) pixels[dest] = shade(pixels[dest],gain);
    }
  }
  void restore(uint16_t *pixels, bool compact, int left, int top, int right, int bottom) const {
    for (int y = top; y < bottom; ++y) {
      int dest = position(left,y,compact);
      for (int x = left; x < right; ++x, ++dest) pixels[dest] = background(x,y);
    }
  }
};
#undef FLIP_OPT
