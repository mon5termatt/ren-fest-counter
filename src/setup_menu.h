#pragma once

#include <Arduino.h>

namespace SetupMenu {
  void begin();
  bool active();
  void enter();
  void exit();
  // Returns true if the press was consumed by the menu.
  bool handleButton(uint8_t buttonId);
}
