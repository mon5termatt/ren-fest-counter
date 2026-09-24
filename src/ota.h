#pragma once

#include <Arduino.h>

namespace Ota {
  void begin();
  void loop();
  bool busy();  // true while an OTA transfer is in progress
}
