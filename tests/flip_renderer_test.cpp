#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include "flip_renderer.h"

int main() {
  constexpr unsigned size = 135*240;
  FlipRenderer renderer;
  std::vector<uint16_t> guarded(size+2,0xABCD), first(size), moving(size), final(size), swapped(size);
  auto *pixels = guarded.data()+1;
  renderer.setDigits("012345",true,100);
  assert(renderer.needsFrame(0,100));
  renderer.render(0,100,first.data());
  renderer.render(1,100,moving.data());
  assert(!renderer.needsFrame(0,101));  // First synchronization is immediately readable.
  renderer.setDigits("912345",true,1000);
  assert(renderer.needsFrame(0,1000));
  assert(!renderer.needsFrame(1,1000));  // Unchanged cards stay idle after rendering.
  renderer.render(0,1100,moving.data());
  renderer.render(0,1600,final.data());
  assert(first != moving && moving != final && first != final);
  assert(!renderer.needsFrame(0,1601));

  // Re-render the same transition in all destination formats and at every phase.
  std::vector<uint16_t> compact(FlipRenderer::kW*FlipRenderer::kH+2,0xABCD);
  for (unsigned digit = 0; digit < 10; ++digit) {
    char digits[] = "000000";
    digits[0] += digit;
    const uint32_t start = 2000+digit*1000;
    renderer.setDigits(digits,true,start);
    for (unsigned age : {0U,1U,99U,199U,200U,201U,299U,399U,400U,450U,519U,520U,900U}) {
      renderer.render(0,start+age,pixels);
      assert(guarded.front() == 0xABCD && guarded.back() == 0xABCD);
      renderer.renderRegion(0,start+age,compact.data()+1);
      assert(compact.front() == 0xABCD && compact.back() == 0xABCD);
      for (int y = 0; y < FlipRenderer::kH; ++y) {
        for (int x = 0; x < FlipRenderer::kW; ++x) {
          assert(compact[1+y*FlipRenderer::kW+x] ==
                 pixels[(y+FlipRenderer::kY)*135+x+FlipRenderer::kX]);
        }
      }
      renderer.render(0,start+age,swapped.data(),true,true);
      for (unsigned n = 0; n < size; ++n) {
        assert(swapped[n] == static_cast<uint16_t>((pixels[n]>>8)|(pixels[n]<<8)));
      }
      memcpy(moving.data(),pixels,size*2);
      renderer.render(0,start+age,moving.data(),false);
      assert(memcmp(pixels,moving.data(),size*2) == 0);
    }
  }
  renderer.setDigits("987654",true,UINT32_MAX-50);
  renderer.render(0,100,pixels);  // Moving leaves survive millis() rollover.
  renderer.render(0,600,pixels);
  assert(!renderer.needsFrame(0,600));
  renderer.reset();
  renderer.setDigits("------",false,700);
  renderer.render(5,800,pixels);
  memcpy(first.data(),pixels,size*2);
  renderer.render(6,800,pixels);
  assert(memcmp(first.data(),pixels,size*2) == 0 && !renderer.needsFrame(6,800));
}
