#pragma once
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <ctime>
inline size_t strlcpy(char *out, const char *in, size_t size) {
  size_t length = strlen(in);
  if (size) { size_t n = length < size - 1 ? length : size - 1; memcpy(out, in, n); out[n] = 0; }
  return length;
}
extern int starts;
inline void configTzTime(const char *tz, const char *) {
  ++starts; setenv("TZ", tz, 1); tzset();
}
