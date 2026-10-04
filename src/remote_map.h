#pragma once

#include <Arduino.h>

namespace RemoteMap {
  enum Action : uint8_t {
    ACTION_NONE = 0,
    ACTION_SELECT = 1,      // param = counter index 0..MAX-1
    ACTION_INC_ACTIVE = 2,
    ACTION_DEC_ACTIVE = 3,
    ACTION_BLANK = 4,
    ACTION_UNBLANK = 5,
    ACTION_SCROLLER = 6,    // toggle cycling marquee of all scrollMessages
    ACTION_IDLE_START = 7,  // start idle playlist immediately (toggle off if running)
  };

  struct Binding {
    uint8_t buttonId;
    uint8_t action;
    uint8_t param;
    uint8_t effectIn;   // ShowMode for enter / action play
    uint8_t effectOut;  // ShowMode for exit transition
    uint8_t speed;      // 1–20, or 0 = use per-anim default
  };

  void begin();
  void loop();  // no-op (kept for call sites)
  void load();
  void save();
  void factoryDefaults();

  void handleButton(uint8_t buttonId);

  uint8_t bindingCount();
  Binding getBinding(uint8_t index);
  bool setBinding(uint8_t buttonId, uint8_t action, uint8_t param,
                  uint8_t effectIn, uint8_t effectOut, uint8_t speed = 0);

  const uint8_t* knownButtons(uint8_t& count);
  const char* buttonLabel(uint8_t buttonId);
  const char* actionLabel(uint8_t action);
}
