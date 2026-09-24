#pragma once

#include <Arduino.h>

namespace WizRemote {
  void begin();
  void loop();  // process queued button into RemoteMap

  // Last seen sender MAC as 12 uppercase hex chars (no separators)
  const char* lastSeenMac();
  bool hasLastSeenMac();
}
