#pragma once

#include <Arduino.h>

namespace WizRemote {
  void begin();
  void loop();  // process queued button into RemoteMap

  // Pop one queued button without dispatching (splash / combo). false if none.
  bool takeButton(uint8_t& buttonOut);

  // Last seen sender MAC as 12 uppercase hex chars (no separators)
  const char* lastSeenMac();
  bool hasLastSeenMac();
}
