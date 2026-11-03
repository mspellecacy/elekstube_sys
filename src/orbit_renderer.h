#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "orbit_assets.h"

#if defined(__GNUC__)
#define ORBIT_OPT __attribute__((optimize("O3")))
#define ORBIT_INLINE inline __attribute__((always_inline))
#else
#define ORBIT_OPT
#define ORBIT_INLINE inline
#endif

// Original procedural clock, shared by the ESP32 and the desktop preview.
class OrbitRenderer {
  struct Target { uint16_t *pixels; int firstY, lastY, originY; };
 public:
  static constexpr int kWidth = 135, kHeight = 240;
  static constexpr int kX = 0, kY = 24, kW = 135, kH = 192;
  static constexpr int kBands = 2, kBandH = kH/kBands;
  static constexpr int kGlyphX = 23, kGlyphY = 49, kGlyphW = 89, kGlyphH = 144;
  static constexpr uint32_t kTransition = 480;

  // Cache only the active face so the Wi-Fi stack has room to run.
  void begin() { current_.allocate(); previous_.allocate(); }
  void end() { current_.release(); previous_.release(); }
  bool cached() const { return current_.ready() && previous_.ready(); }
  OrbitRenderer() {
    // Geometry and emission are invariant. Keep these small tables in RAM so
    // each tube doesn't repeat 292 trig calls and 32 exponentials every frame.
    for (unsigned n = 0; n <= 72; ++n) {
      const float angle = n*(6.2831853f/72);
      circleX_[n] = cosf(angle);
      circleY_[n] = sinf(angle);
    }
    static const float distances[] = {0,0.25f,0.5f,0.75f,1,1.25f,1.5f,1.75f,2,2.5f,3,4,5.5f,7.5f,11,16};
    for (unsigned d = 0; d < 16; ++d) {
      const float range = distances[d];
      const float core = expf(-range*range/1.3f), halo = expf(-range/3.4f);
      emission_[d][0] = 206*core+3*halo;
      emission_[d][1] = 230*core+50*halo;
      emission_[d][2] = 240*core+110*halo;
    }
    for (unsigned n = 0; n < 64; ++n) {
      cyan_[n] = light(45,205,255,n/63.0f);
      violet_[n] = light(157,101,255,n/63.0f);
    }
    // Tiny RAM tables synthesize the midnight-blue field without streaming a
    // whole background from flash on every tube refresh.
    for (int x = 0; x < kWidth; ++x) {
      const float t = (x-67)/70.0f;
      horizontal_[x] = static_cast<uint8_t>(15*expf(-t*t)+0.5f);
    }
    for (int y = 0; y < kH; ++y) {
      const float t = (y+kY-120)/108.0f;
      vertical_[y] = static_cast<uint8_t>(15*expf(-t*t)+0.5f);
    }
    const unsigned bayer[] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    for (unsigned energy = 0; energy < 16; ++energy) for (unsigned n = 0; n < 16; ++n) {
      const float t = energy/15.0f, threshold = bayer[n]/16.0f;
      const unsigned r = (2+4*t)*31/255+threshold, g = (4+6*t)*63/255+threshold,
                     b = (9+14*t)*31/255+threshold;
      background_[energy][n] = (r<<11)|(g<<5)|b;
    }
  }
  OrbitRenderer(const OrbitRenderer &) = delete;
  OrbitRenderer &operator=(const OrbitRenderer &) = delete;
  void reset() { for (auto &tube : tubes_) tube = Tube{}; }
  void setDigits(const char *digits, bool synced, uint32_t now) {
    for (unsigned i = 0; i < 6; ++i) {
      const int target = synced && digits[i] >= '0' && digits[i] <= '9' ? digits[i]-'0' : -1;
      auto &tube = tubes_[i];
      if (target != tube.digit) {
        tube.previous = tube.digit;
        tube.digit = target;
        tube.changed = now;
      }
    }
  }

  ORBIT_OPT void render(unsigned index, uint32_t now, uint16_t *pixels,
                        bool full = true, bool swapped = false, bool compact = false, int band = -1) const {
    if (index >= 6 || band >= kBands) return;
    const int first = band < 0 ? kY : kY+band*kBandH;
    const int last = band < 0 ? kY+kH : first+kBandH;
    const Target target{pixels,first,last,compact ? first : 0};
    const int startY = full ? 0 : first, endY = full ? kHeight : last;
    // Static title and trim are copied only on a full redraw.
    if (full) memcpy(pixels,kOrbitBackground,kWidth*kHeight*sizeof(uint16_t));
    paintBackground(target);
    const auto &tube = tubes_[index];
    const uint32_t age = now-tube.changed;
    const bool changing = tube.previous >= 0 && tube.digit >= 0 && age < kTransition;
    const float flare = changing ? 0.18f*sinf(age*(3.14159265f/kTransition)) : 0;
    const float breathing = 0.91f+0.07f*sinf((now%5600)*(6.2831853f/5600)-index*0.44f)+flare;
    for (unsigned n = 0; n < 14; ++n) {
      const unsigned period = 2100+n*257;
      const float twinkle = 0.08f+0.11f*(1+sinf((now%period)*(6.2831853f/period)+n*2.4f+index));
      const int x = 14+(n*41+index*7)%108, y = 34+(n*59+index*13)%171;
      dot(target,x,y,1.5f,twinkle,cyan_);
    }
    Ring rings[2];
    for (unsigned r = 0; r < 2; ++r) {
      auto &ring = rings[r];
      const unsigned period = r ? 51000 : 39000;
      const float phase = (now%period)*(r ? -6.2831853f/period : 6.2831853f/period)+index*0.48f+r*1.9f;
      const float roll = 0.24f*sinf(phase+r*1.6f);
      const float radius = 34+20*sinf(phase)*sinf(phase);
      const float cosine = cosf(roll), sine = sinf(roll);
      const unsigned revolution = r ? 9600 : 14400;
      ring.head = fmodf((now%revolution)*(72.0f/revolution)+index*11+r*29,72);
      for (unsigned n = 0; n <= 72; ++n) {
        const float x = radius*circleX_[n], y = (r ? 79 : 85)*circleY_[n];
        ring.x[n] = 67+x*cosine-y*sine;
        ring.y[n] = 120+x*sine+y*cosine;
      }
    }
    for (unsigned r = 0; r < 2; ++r) orbit(target,rings[r],false,r ? violet_ : cyan_,1+flare);
    numeral(target,tube,age,changing,breathing);
    for (unsigned r = 0; r < 2; ++r) {
      const auto *palette = r ? violet_ : cyan_;
      orbit(target,rings[r],true,palette,1+flare);
      const unsigned head = static_cast<unsigned>(rings[r].head);
      const float amount = rings[r].head-head;
      dot(target,rings[r].x[head]+amount*(rings[r].x[head+1]-rings[r].x[head]),
          rings[r].y[head]+amount*(rings[r].y[head+1]-rings[r].y[head]),7,0.85f,palette);
    }
    if (swapped) {
      const unsigned offset = compact ? 0 : startY*kWidth;
      for (unsigned n = offset; n < offset+(endY-startY)*kWidth; ++n) pixels[n] = swap(pixels[n]);
    }
  }
  void renderRegion(unsigned index, uint32_t now, uint16_t *pixels, bool swapped = false) const {
    render(index,now,pixels,false,swapped,true);
  }
  // A band writes only its own compact output. The other half of the sprite
  // can remain in flight to a display while this half is being composed.
  void renderBand(unsigned index, uint32_t now, unsigned band, uint16_t *pixels, bool swapped = false) const {
    if (band >= kBands) return;
    render(index,now,pixels,false,swapped,true,band);
  }

 private:
  struct Tube { int digit = -1, previous = -1; uint32_t changed = 0; };
  struct Ring { float x[73],y[73],head; };
  struct GlyphCache {
    uint8_t *parts[2] = {};
    mutable int key = -1;
    ~GlyphCache() { release(); }
    void release() { for (auto &part : parts) { free(part); part = nullptr; } key = -1; }
    void allocate() {
      for (auto &part : parts) if (!part) part = static_cast<uint8_t *>(malloc(kGlyphW*kGlyphH/4));
      key = -1;
    }
    bool ready() const { return parts[0] && parts[1]; }
    const uint8_t *row(int digit, int y) const {
      const auto *source = kOrbitGlyphs+digit*kGlyphW*kGlyphH/2;
      if (digit != key) {
        for (unsigned n = 0; n < 2; ++n) if (parts[n]) memcpy(parts[n],source+n*kGlyphW*kGlyphH/4,kGlyphW*kGlyphH/4);
        key = digit;
      }
      const unsigned half = y/(kGlyphH/2);
      return parts[half] ? parts[half]+(y%(kGlyphH/2))*kGlyphW/2 : source+y*kGlyphW/2;
    }
  };
  Tube tubes_[6];
  GlyphCache current_, previous_;
  uint8_t horizontal_[kWidth], vertical_[kH];
  uint16_t background_[16][16];
  float circleX_[73], circleY_[73], emission_[16][3];
  uint16_t cyan_[64], violet_[64];

  ORBIT_OPT void paintBackground(const Target &target) const {
    for (int y = target.firstY; y < target.lastY; ++y) {
      const unsigned row = y%4*4;
      auto *dest = target.pixels+(y-target.originY)*kWidth;
      for (int x = 0; x < kWidth; ++x) {
        dest[x] = background_[(horizontal_[x]*vertical_[y-kY]+7)/15][row+x%4];
      }
    }
    const uint16_t rail = light(12,27,41,1), tick = light(22,47,61,1);
    for (int y = 49; y <= 192; ++y) {
      if (y < target.firstY || y >= target.lastY) continue;
      const unsigned dest = (y-target.originY)*kWidth;
      target.pixels[dest+5] = target.pixels[dest+129] = rail;
      if (y >= 56 && (y-56)%8 == 0) {
        const unsigned length = y%24 == 8 ? 3 : 1;
        for (unsigned n = 0; n <= length; ++n) target.pixels[dest+5+n] = target.pixels[dest+129-n] = tick;
      }
    }
  }

  static unsigned limit(float v) { return v < 0 ? 0 : v > 63 ? 63 : static_cast<unsigned>(v); }
  static unsigned distance(const uint8_t *row, int x, int y) {
    return (row[(x+(y&1))/2] >> ((x+y)&1 ? 0 : 4))&15;
  }
  static float smooth(float v) { return v <= 0 ? 0 : v >= 1 ? 1 : v*v*(3-2*v); }
  static uint16_t light(float r, float g, float b, float gain) {
    const unsigned red = limit(r*gain*31/255), green = limit(g*gain*63/255), blue = limit(b*gain*31/255);
    return ((red > 31 ? 31 : red)<<11)|(green<<5)|(blue > 31 ? 31 : blue);
  }
  static uint16_t swap(uint16_t v) { return (v>>8)|(v<<8); }
  static uint16_t add(uint16_t a, uint16_t b) {
    uint32_t rb = (a&0xF81F)+(b&0xF81F), g = (a&0x07E0)+(b&0x07E0);
    const uint32_t carry = rb&0x10020, green = g&0x0800;
    rb |= carry-(carry>>5); g |= green-(green>>6);
    return (rb&0xF81F)|(g&0x07E0);
  }
  static ORBIT_INLINE void pixel(const Target &target, int x, int y, uint16_t color) {
    if (x < kX || x >= kX+kW || y < target.firstY || y >= target.lastY) return;
    const unsigned n = (y-target.originY)*kWidth+x;
    target.pixels[n] = add(target.pixels[n],color);
  }
  static void dot(const Target &target, float x, float y, float radius,
                  float energy, const uint16_t *palette) {
    if (y+radius < target.firstY || y-radius >= target.lastY) return;
    const float inverse = 1/(radius*radius);
    for (int yy = static_cast<int>(y-radius); yy <= y+radius; ++yy) {
      for (int xx = static_cast<int>(x-radius); xx <= x+radius; ++xx) {
        const float d = ((xx-x)*(xx-x)+(yy-y)*(yy-y))*inverse;
        if (d < 1) {
          const float glow = d < 0.025f ? 1 : 0.42f*(1-d)*(1-d);
          pixel(target,xx,yy,palette[limit(energy*glow*63)]);
        }
      }
    }
  }
  static ORBIT_OPT void line(const Target &target, float x0, float y0, float x1, float y1,
                             float energy, const uint16_t *palette) {
    // A band needn't rasterize the other half of each orbit. Keep a three-pixel
    // margin for antialiasing and the outer light splats at the boundary.
    if ((y0+3 < target.firstY && y1+3 < target.firstY) ||
        (y0-3 >= target.lastY && y1-3 >= target.lastY)) return;
    // Fixed-point light splats replace a floating-point distance calculation at
    // every pixel. Bilinear coverage keeps the moving wire antialiased.
    int x = static_cast<int>(x0*256), y = static_cast<int>(y0*256);
    const int dx = static_cast<int>((x1-x0)*256), dy = static_cast<int>((y1-y0)*256);
    const int ax = abs(dx), ay = abs(dy), steps = ((ax > ay ? ax : ay)+255)/256;
    if (!steps) return;
    const int sx = dx/steps, sy = dy/steps;
    const unsigned strength = limit(energy*63);
    const uint16_t halo = palette[strength/12];
    for (int n = 0; n < steps; ++n,x+=sx,y+=sy) {
      const int xx = x>>8, yy = y>>8;
      const unsigned fx = x&255, fy = y&255;
      pixel(target,xx,yy,palette[(strength*(256-fx)*(256-fy)+32768)>>16]);
      pixel(target,xx+1,yy,palette[(strength*fx*(256-fy)+32768)>>16]);
      pixel(target,xx,yy+1,palette[(strength*(256-fx)*fy+32768)>>16]);
      pixel(target,xx+1,yy+1,palette[(strength*fx*fy+32768)>>16]);
      if (halo) {
        pixel(target,xx-1,yy,halo);
        pixel(target,xx+2,yy+1,halo);
        pixel(target,xx,yy-1,halo);
        pixel(target,xx+1,yy+2,halo);
      }
    }
  }
  static void orbit(const Target &target, const Ring &ring, bool front,
                    const uint16_t *palette, float gain) {
    for (unsigned n = 0; n < 72; ++n) {
      if ((n < 36) != front) continue;
      float tail = ring.head-n;
      if (tail < 0) tail += 72;
      const float trail = tail < 20 ? (20-tail)/20.0f : 0;
      const float energy = (0.075f+0.60f*trail*trail)*gain*(front ? 0.85f : 0.55f);
      line(target,ring.x[n],ring.y[n],ring.x[n+1],ring.y[n+1],energy,palette);
    }
  }
  ORBIT_OPT void numeral(const Target &target, const Tube &tube, uint32_t age,
                        bool changing, float breathing) const {
    if (tube.digit < 0) return;
    uint16_t live[16][16], old[16];
    // Per-frame palettes turn the hot loop into RAM reads and saturated addition.
    for (unsigned d = 0; d < 16; ++d) {
      const float red = emission_[d][0], green = emission_[d][1], blue = emission_[d][2];
      for (unsigned level = changing ? 0 : 15; level < 16; ++level)
        live[level][d] = light(red,green,blue,breathing*level/15);
      if (changing) old[d] = light(red*0.5f,green*0.6f,blue,(1-smooth(age/200.0f))*0.7f);
    }
    const float scan = changing ? (1-age/float(kTransition))*(kGlyphH+28)-14 : -14;
    const int first = target.firstY > kGlyphY ? target.firstY-kGlyphY : 0;
    const int last = target.lastY < kGlyphY+kGlyphH ? target.lastY-kGlyphY : kGlyphH;
    for (int y = first; y < last; ++y) {
      const auto *current = current_.row(tube.digit,y);
      const auto *previous = changing ? previous_.row(tube.previous,y) : current;
      const unsigned level = changing ? static_cast<unsigned>(15*smooth((y-scan)/18+0.5f)) : 15;
      unsigned dest = (y+kGlyphY-target.originY)*kWidth+kGlyphX;
      for (int x = 0; x < kGlyphW; ++x, ++dest) {
        uint16_t color = live[level][distance(current,x,y)];
        if (changing) color = add(color,old[distance(previous,x,y)]);
        target.pixels[dest] = add(target.pixels[dest],color);
      }
    }
  }
};
#undef ORBIT_OPT
#undef ORBIT_INLINE
