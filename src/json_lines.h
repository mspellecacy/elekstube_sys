#pragma once
#include <stddef.h>

// Independent bounded framing for USB and TCP, including recovery after overflow.
template <size_t Capacity> struct JsonLines {
  enum Result { Pending, Complete, TooLong };
  char data[Capacity] = {};
  size_t length = 0;
  bool dropping = false;
  void reset() { length = 0; dropping = false; }
  Result push(char c) {
    if (c == '\n') {
      data[length] = '\0';
      return dropping ? TooLong : length ? Complete : Pending;
    }
    if (c != '\r' && !dropping) {
      if (length < Capacity-1) data[length++] = c;
      else dropping = true;
    }
    return Pending;
  }
};
