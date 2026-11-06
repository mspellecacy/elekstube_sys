#pragma once
#include <cstring>
struct Preferences {
  static unsigned char data[224];
  static bool saved, fail;
  bool begin(const char *, bool) { return true; }
  void end() {}
  size_t putBytes(const char *, const void *value, size_t size) {
    if (fail) return 0;
    memcpy(data, value, size); saved = true; return size;
  }
  size_t getBytesLength(const char *) { return saved ? sizeof(data) : 0; }
  size_t getBytes(const char *, void *value, size_t size) {
    memcpy(value, data, size); return size;
  }
};
