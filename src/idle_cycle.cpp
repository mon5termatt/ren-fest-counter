#include "idle_cycle.h"
#include "config.h"
#include "counters.h"
#include "display.h"

namespace {
  uint32_t lastActivityMs = 0;
  uint32_t lastCycleMs = 0;
  bool cycling = false;

  int8_t nextEnabled(uint8_t from) {
    for (uint8_t step = 1; step <= MAX_COUNTERS; step++) {
      uint8_t i = (from + step) % MAX_COUNTERS;
      if (Counters::isEnabled(i)) return static_cast<int8_t>(i);
    }
    return -1;
  }

  uint8_t enabledTotal() {
    return Counters::enabledCount();
  }
}

namespace IdleCycle {

void begin() {
  lastActivityMs = millis();
  lastCycleMs = millis();
  cycling = false;
}

void noteActivity() {
  lastActivityMs = millis();
  if (cycling) {
    cycling = false;
    Serial.println(F("[idle] activity — stop cycle"));
  }
}

bool isCycling() { return cycling; }

void loop() {
  if (Display::scrollerActive()) {
    return;
  }

  if (enabledTotal() < 2) {
    cycling = false;
    return;
  }

  // Don't cycle while blanked fully off
  if (Counters::blanked() && Counters::blankBrightnessPercent() == 0) {
    return;
  }

  const uint32_t now = millis();

  if (!cycling) {
    if (now - lastActivityMs >= IDLE_TIMEOUT_MS) {
      cycling = true;
      lastCycleMs = now;
      Serial.println(F("[idle] start counter cycle"));
      // Advance once immediately so it feels like attract mode kicked in
      int8_t n = nextEnabled(Counters::activeIndex());
      if (n >= 0) {
        Counters::setActiveIndex(static_cast<uint8_t>(n), false);
        Display::idleShowNext(Counters::getConst(n).count, Counters::idleEffect());
      }
    }
    return;
  }

  if (now - lastCycleMs >= (uint32_t)Counters::idleCycleSeconds() * 1000UL) {
    lastCycleMs = now;
    int8_t n = nextEnabled(Counters::activeIndex());
    if (n >= 0) {
      Counters::setActiveIndex(static_cast<uint8_t>(n), false);
      Display::idleShowNext(Counters::getConst(n).count, Counters::idleEffect());
    }
  }
}

}  // namespace IdleCycle
