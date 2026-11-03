// Host driver for the exact embedded renderer. Emits RGB565 frame buffers.
#include <stdio.h>
#include <stdlib.h>
#include "nixie_renderer.h"

int main(int argc, char **argv) {
  NixieRenderer renderer;
  renderer.begin();
  uint16_t pixels[135*240];
  const unsigned count = argc > 1 ? static_cast<unsigned>(atoi(argv[1])) : 1;
  for (unsigned frame = 0; frame < count; ++frame) {
    const uint32_t now = 1000+frame*80;
    char digits[] = "235956";
    unsigned seconds = 23*3600+59*60+56+frame*80/1000;
    seconds %= 86400;
    const unsigned parts[] = {seconds/3600, seconds/60%60, seconds%60};
    for (unsigned p = 0; p < 3; ++p) {
      digits[p*2] = '0'+parts[p]/10;
      digits[p*2+1] = '0'+parts[p]%10;
    }
    renderer.setDigits(digits,true,now);
    for (unsigned tube = 0; tube < 6; ++tube) {
      renderer.render(tube,now,pixels);
      if (fwrite(pixels,sizeof(pixels),1,stdout) != 1) return 1;
    }
  }
}
