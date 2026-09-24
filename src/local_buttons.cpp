#include "local_buttons.h"
#include "config.h"
#include "counters.h"
#include "display.h"
#include "idle_cycle.h"

namespace {
  enum BtnId : uint8_t { BTN_DEC = 0, BTN_INC = 1 };

  struct BtnState {
    int8_t pin;
    bool stablePressed;
    bool rawPressed;
    uint32_t lastChangeMs;
    uint32_t pressStartMs;
    uint32_t lastRepeatMs;
  };

  BtnState decBtn;
  BtnState incBtn;

  bool pinUsable(int8_t pin) {
    if (pin < 0) return false;
    if (pin >= 34) {
      Serial.printf("[btn] refusing GPIO %d (no internal pull-up)\n", pin);
      return false;
    }
    return true;
  }

  void initBtn(BtnState& b, int8_t pin) {
    b.pin = pinUsable(pin) ? pin : (int8_t)-1;
    b.stablePressed = false;
    b.rawPressed = false;
    b.lastChangeMs = 0;
    b.pressStartMs = 0;
    b.lastRepeatMs = 0;
    if (b.pin >= 0) {
      pinMode(b.pin, INPUT_PULLUP);
      if (digitalRead(b.pin) == LOW) {
        Serial.printf("[btn] GPIO %d stuck LOW at boot — waiting for release\n", b.pin);
        b.stablePressed = true;
        b.rawPressed = true;
      }
    }
  }

  bool readPressed(const BtnState& b) {
    if (b.pin < 0) return false;
    return digitalRead(b.pin) == LOW;
  }

  void fire(BtnId which) {
    IdleCycle::noteActivity();
    const uint8_t a = Counters::activeIndex();
    if (which == BTN_INC) {
      if (Counters::inc(a)) {
        Display::onCountIncreased(Counters::getConst(a).count,
                                  Counters::localIncEffect());
      }
    } else {
      if (Counters::dec(a)) {
        Display::onCountDecreased(Counters::getConst(a).count,
                                  Counters::localDecEffect());
      }
    }
  }

  void pollBtn(BtnState& b, BtnId which) {
    if (b.pin < 0) return;

    const uint32_t now = millis();
    const bool raw = readPressed(b);

    if (raw != b.rawPressed) {
      b.rawPressed = raw;
      b.lastChangeMs = now;
    }

    if ((now - b.lastChangeMs) < BTN_DEBOUNCE_MS) return;

    if (raw && !b.stablePressed) {
      b.stablePressed = true;
      b.pressStartMs = now;
      b.lastRepeatMs = now;
      fire(which);
    } else if (!raw && b.stablePressed) {
      b.stablePressed = false;
    } else if (raw && b.stablePressed) {
      if ((now - b.pressStartMs) >= BTN_HOLD_MS &&
          (now - b.lastRepeatMs) >= BTN_REPEAT_MS) {
        b.lastRepeatMs = now;
        fire(which);
      }
    }
  }
}

namespace LocalButtons {

void begin() {
  initBtn(decBtn, BTN_DEC_PIN);
  initBtn(incBtn, BTN_INC_PIN);
}

void loop() {
  pollBtn(decBtn, BTN_DEC);
  pollBtn(incBtn, BTN_INC);
}

}  // namespace LocalButtons
