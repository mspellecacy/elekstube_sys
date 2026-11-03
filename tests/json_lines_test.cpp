#include <assert.h>
#include <string.h>
#include "json_lines.h"

int main() {
  JsonLines<8> usb, tcp;
  for (char c : "1234567") if (c) assert(usb.push(c) == JsonLines<8>::Pending);
  assert(tcp.push('x') == JsonLines<8>::Pending);
  assert(usb.push('\r') == JsonLines<8>::Pending);
  assert(usb.push('\n') == JsonLines<8>::Complete);
  assert(strcmp(usb.data,"1234567") == 0);
  assert(tcp.push('\n') == JsonLines<8>::Complete);
  assert(strcmp(tcp.data,"x") == 0);  // A partial TCP line never mixes with USB.
  usb.reset(); tcp.reset();
  assert(usb.push('\n') == JsonLines<8>::Pending);
  for (int i = 0; i < 10000; ++i) usb.push('a');
  assert(usb.push('\n') == JsonLines<8>::TooLong);
  usb.reset();
  usb.push('b');
  assert(usb.push('\n') == JsonLines<8>::Complete);
  assert(strcmp(usb.data,"b") == 0);  // A complete next line recovers.
}
