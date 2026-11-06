#pragma once
constexpr int WL_CONNECTED = 3;
struct TestWifi { int connection = 0; int status() { return connection; } };
extern TestWifi WiFi;
