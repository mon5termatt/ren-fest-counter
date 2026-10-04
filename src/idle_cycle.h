#pragma once

#include <Arduino.h>

namespace IdleCycle {
  void begin();
  void loop();
  void noteActivity();  // call on any user input (remote, buttons, web)
  bool startNow();      // begin playlist immediately (remote / web)
  bool isCycling();
}
