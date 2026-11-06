#include <sys/time.h>
static timeval sample;
static int testGettimeofday(timeval *value, void *) { *value = sample; return 0; }
#define gettimeofday testGettimeofday
#include "ntp_clock.h"
#undef gettimeofday
#include <cassert>
#include <string>
int starts = 0;
TestWifi WiFi;
unsigned char Preferences::data[224] = {};
bool Preferences::saved = false, Preferences::fail = false;
int main() {
  NtpClock ntp;
  LocalClock clock;
  ntp.begin();
  assert(!ntp.poll(clock, 0) && !clock.synced());
  WiFi.connection = WL_CONNECTED;
  assert(!ntp.poll(clock, 0) && starts == 1);
  assert(!ntp.poll(clock, 1) && starts == 1);
  sample = {1710053999, 500000}; // 2024-03-10 06:59:59.500 UTC
  assert(ntp.configure("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org"));
  assert(ntp.poll(clock, 2) && ntp.synced());
  char digits[7]; clock.digits(digits);
  assert(std::string(digits) == "015959");
  sample.tv_sec++;
  ntp.poll(clock, 3); clock.digits(digits);
  assert(std::string(digits) == "030000"); // DST spring transition
  WiFi.connection = 0;
  sample.tv_sec++;
  assert(ntp.poll(clock, 4)); clock.digits(digits);
  assert(std::string(digits) == "030001");
  assert(!ntp.configure("", "pool.ntp.org"));
  assert(!ntp.configure("UTC0", "bad server"));
  Preferences::fail = true;
  assert(!ntp.configure("UTC0", "other.example"));
  Preferences::fail = false;
  NtpClock restored; restored.begin();
  WiFi.connection = WL_CONNECTED;
  restored.poll(clock, 5); clock.digits(digits);
  assert(std::string(digits) == "030001"); // Failed write preserves saved settings
}
