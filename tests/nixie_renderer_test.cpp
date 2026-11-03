#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include "nixie_renderer.h"

int main() {
  constexpr unsigned size = 135*240;
  NixieRenderer renderer;
  renderer.begin();
  assert(renderer.cached());
  renderer.begin();  // Firmware retries initialization after startup allocations.
  assert(renderer.cached());
  std::vector<uint16_t> guarded(size+2, 0xABCD), first(size), later(size), swapped(size);
  uint16_t *pixels = guarded.data()+1;
  renderer.setDigits("012345",true,100);
  renderer.render(0,500,pixels);
  assert(guarded.front() == 0xABCD && guarded.back() == 0xABCD);
  memcpy(first.data(),pixels,size*2);
  renderer.end(); renderer.end();
  assert(!renderer.cached());
  renderer.render(0,500,later.data());
  assert(first == later);
  renderer.begin();
  assert(renderer.cached());
  renderer.render(0,500,later.data());
  assert(first == later);  // Cache release/re-entry preserves the face exactly.
  renderer.render(0,2000,later.data());
  assert(first != later);  // A stationary digit's light continuously breathes.

  renderer.render(0,500,swapped.data(),true,true);
  for (unsigned n = 0; n < size; ++n) {
    assert(swapped[n] == static_cast<uint16_t>((first[n]>>8)|(first[n]<<8)));
  }
  renderer.render(0,2000,pixels,false);
  assert(memcmp(pixels,later.data(),size*2) == 0);  // Cropped refresh matches full frame.
  assert(guarded.front() == 0xABCD && guarded.back() == 0xABCD);

  std::vector<uint16_t> compact(NixieRenderer::kGlowW*NixieRenderer::kGlowH+2,0xABCD);
  renderer.renderGlow(0,2000,compact.data()+1);
  for (int y = 0; y < NixieRenderer::kGlowH; ++y) {
    for (int x = 0; x < NixieRenderer::kGlowW; ++x) {
      assert(compact[1+y*NixieRenderer::kGlowW+x] ==
             later[(y+NixieRenderer::kGlowY)*135+x+NixieRenderer::kGlowX]);
    }
  }
  assert(compact.front() == 0xABCD && compact.back() == 0xABCD);

  // All ten distance fields stay in bounds, including during old/new overlap.
  for (unsigned digit = 0; digit < 10; ++digit) {
    char digits[] = "000000";
    digits[0] += digit;
    renderer.setDigits(digits,true,3000+digit*500);
    for (unsigned age : {0U,40U,60U,100U,219U,220U,400U}) {
      renderer.render(0,3000+digit*500+age,pixels);
      assert(guarded.front() == 0xABCD && guarded.back() == 0xABCD);
    }
  }
  renderer.reset();
  renderer.setDigits("987654",true,UINT32_MAX-50);
  renderer.render(0,100,pixels);  // Ignition survives millis() rollover.
  renderer.setDigits("------",false,200);
  renderer.render(5,500,pixels);  // Unsynchronized tubes have no active cathode.
  renderer.render(6,500,pixels);  // Invalid index does not write any buffer.
  assert(guarded.front() == 0xABCD && guarded.back() == 0xABCD);
}
