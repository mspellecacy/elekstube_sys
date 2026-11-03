#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include "orbit_renderer.h"

int main() {
  constexpr unsigned size = 135*240;
  OrbitRenderer renderer;
  std::vector<uint16_t> guarded(size+2,0xABCD), fallback(size), first(size), later(size), swapped(size);
  auto *pixels = guarded.data()+1;
  renderer.setDigits("012345",true,100);
  renderer.render(0,100,fallback.data());  // Flash fallback before caches exist.
  assert(!renderer.cached());
  renderer.begin();
  assert(renderer.cached());
  renderer.render(0,100,first.data());
  assert(fallback == first);
  renderer.end(); renderer.end();
  assert(!renderer.cached());
  renderer.render(0,100,later.data());
  assert(first == later);
  renderer.begin();  // Initialization is idempotent and reloads cache keys safely.
  assert(renderer.cached());
  renderer.render(0,100,later.data());
  assert(first == later);
  renderer.render(0,900,later.data());
  assert(first != later);  // Orbit and stars move even when this digit stays fixed.

  std::vector<uint16_t> compact(OrbitRenderer::kW*OrbitRenderer::kH+2,0xABCD);
  constexpr unsigned bandPixels = OrbitRenderer::kW*OrbitRenderer::kBandH;
  std::vector<uint16_t> band(bandPixels+2,0xABCD);
  for (unsigned digit = 0; digit < 10; ++digit) {
    char digits[] = "000000";
    for (unsigned n = 0; n < 6; ++n) digits[n] += (digit+n)%10;
    const unsigned tube = digit%6;
    digits[tube] = '0'+digit;
    const uint32_t start = 2000+digit*1000;
    renderer.setDigits(digits,true,start);
    for (unsigned age : {0U,1U,100U,199U,200U,240U,479U,480U,900U}) {
      renderer.render(tube,start+age,pixels);
      assert(guarded.front() == 0xABCD && guarded.back() == 0xABCD);
      renderer.renderRegion(tube,start+age,compact.data()+1);
      assert(compact.front() == 0xABCD && compact.back() == 0xABCD);
      for (int y = 0; y < OrbitRenderer::kH; ++y) for (int x = 0; x < OrbitRenderer::kW; ++x) {
        assert(compact[1+y*OrbitRenderer::kW+x] == pixels[(y+OrbitRenderer::kY)*135+x]);
      }
      for (unsigned part = 0; part < OrbitRenderer::kBands; ++part) {
        renderer.renderBand(tube,start+age,part,band.data()+1);
        assert(band.front() == 0xABCD && band.back() == 0xABCD);
        assert(memcmp(band.data()+1,compact.data()+1+part*bandPixels,bandPixels*2) == 0);
        renderer.renderBand(tube,start+age,part,band.data()+1,true);
        for (unsigned n = 0; n < bandPixels; ++n) {
          const auto pixel = compact[1+part*bandPixels+n];
          assert(band[n+1] == static_cast<uint16_t>((pixel>>8)|(pixel<<8)));
        }
        assert(band.front() == 0xABCD && band.back() == 0xABCD);
      }
      renderer.render(tube,start+age,swapped.data(),true,true);
      for (unsigned n = 0; n < size; ++n) {
        assert(swapped[n] == static_cast<uint16_t>((pixels[n]>>8)|(pixels[n]<<8)));
      }
      memcpy(later.data(),pixels,size*2);
      renderer.render(tube,start+age,later.data(),false);
      assert(memcmp(pixels,later.data(),size*2) == 0);
    }
  }
  renderer.setDigits("987654",true,UINT32_MAX-50);
  renderer.render(0,100,pixels);  // Transition elapsed time crosses millis() wrap.
  renderer.render(0,600,pixels);
  renderer.reset();
  renderer.setDigits("------",false,700);
  renderer.render(5,800,pixels);
  memcpy(first.data(),pixels,size*2);
  renderer.render(6,800,pixels);
  assert(memcmp(first.data(),pixels,size*2) == 0);

  OrbitRenderer settled;
  settled.setDigits("012345",true,100);
  settled.render(0,100,first.data());
  renderer.reset();
  renderer.setDigits("012345",true,100);
  renderer.render(0,100,later.data());
  assert(first == later);  // First sync shows a fully readable numeral immediately.

  // Reference captured before the performance changes: all six tubes, all ten
  // digits and seven transition ages must preserve the original pixels exactly.
  OrbitRenderer golden;
  golden.begin();
  uint64_t framesHash = 0;
  for (unsigned digit = 0; digit < 10; ++digit) {
    char digits[7] = "000000";
    for (unsigned n = 0; n < 6; ++n) digits[n] += (n+digit)%10;
    const unsigned start = 100+digit*1000;
    golden.setDigits(digits,true,start);
    for (unsigned age : {0U,40U,199U,240U,479U,480U,900U}) for (unsigned tube = 0; tube < 6; ++tube) {
      golden.render(tube,start+age,pixels);
      uint64_t frameHash = 14695981039346656037ULL;
      for (unsigned n = 0; n < size; ++n) { frameHash ^= pixels[n]; frameHash *= 1099511628211ULL; }
      framesHash = (framesHash*1099511628211ULL)^frameHash;
    }
  }
  assert(framesHash == 0x39d34f2e7c44590cULL);
}
