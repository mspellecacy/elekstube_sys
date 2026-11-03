// Exact firmware renderer; native RGB565 output for the desktop preview.
#include <stdio.h>
#include <stdlib.h>
#include "orbit_renderer.h"

int main(int argc, char **argv) {
  OrbitRenderer renderer;
  renderer.begin();
  uint16_t pixels[135*240];
  const unsigned count = argc > 1 ? static_cast<unsigned>(atoi(argv[1])) : 1;
  for (unsigned frame = 0; frame < count; ++frame) {
    const uint32_t now = 1000+frame*80;
    const unsigned seconds = (23*3600+59*60+57+frame*80/1000)%86400;
    const unsigned parts[] = {seconds/3600,seconds/60%60,seconds%60};
    char digits[7] = {};
    for (unsigned part = 0; part < 3; ++part) {
      digits[part*2] = '0'+parts[part]/10;
      digits[part*2+1] = '0'+parts[part]%10;
    }
    renderer.setDigits(digits,true,now);
    for (unsigned tube = 0; tube < 6; ++tube) {
      renderer.render(tube,now,pixels);
      if (fwrite(pixels,sizeof(pixels),1,stdout) != 1) return 1;
    }
  }
}
