#pragma once

#include <Arduino.h>

namespace WifiMgr {
  void begin();   // SoftAP always on; join saved STA if present
  void loop();

  // Persist and connect (empty pass OK for open networks)
  bool connectSta(const char* ssid, const char* password);
  void clearSta();
  void disconnectSta();

  bool staConfigured();
  bool staConnected();
  const char* staSsid();
  String staIp();
  String apIp();
  int32_t staRssi();
  const char* staStatusText();

  // Blocking scan; returns network count (0 on failure)
  int16_t scanNetworks();
  // After scanNetworks(): valid while results held (until next scan / delete)
  int16_t scanCount();
  const char* scanSsid(uint8_t i);
  int32_t scanRssi(uint8_t i);
  bool scanSecure(uint8_t i);
}
