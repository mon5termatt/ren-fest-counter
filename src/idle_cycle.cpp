#include "idle_cycle.h"
#include "config.h"
#include "counters.h"
#include "display.h"

namespace {
  uint32_t lastActivityMs = 0;
  uint32_t lastCycleMs = 0;
  bool cycling = false;
  bool idleOwnsScroller = false;
  uint8_t playlistCursor = 0;  // next step to show

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

  bool stepIsValid(const IdleStep& step) {
    if (step.kind == IdleCounter) {
      return Counters::isEnabled(step.index);
    }
    if (step.kind == IdleMessage) {
      if (step.index >= Counters::scrollMessageCount()) return false;
      const char* msg = Counters::scrollMessage(step.index);
      return msg && msg[0];
    }
    return false;
  }

  // Find next valid playlist index starting at `from` (inclusive), wrapping once.
  // Returns -1 if none.
  int8_t nextValidPlaylistIndex(uint8_t from) {
    const uint8_t len = Counters::idlePlaylistLen();
    if (len == 0) return -1;
    for (uint8_t step = 0; step < len; step++) {
      uint8_t i = (from + step) % len;
      if (stepIsValid(Counters::idleStep(i))) return static_cast<int8_t>(i);
    }
    return -1;
  }

  void stopIdleScroller() {
    if (idleOwnsScroller) {
      idleOwnsScroller = false;
      if (Display::scrollerActive()) {
        Display::stopManualScroller(false);
      }
    }
  }

  bool showPlaylistStep(uint8_t idx) {
    IdleStep step = Counters::idleStep(idx);
    if (!stepIsValid(step)) return false;
    const uint8_t effect = step.effect;

    if (step.kind == IdleCounter) {
      stopIdleScroller();
      Counters::setActiveIndex(step.index, false);
      Display::idleShowNext(Counters::getConst(step.index).count, effect);
      return true;
    }

    // Message: idle-owned marquee (Left/Right set direction)
    stopIdleScroller();
    const char* msg = Counters::scrollMessage(step.index);
    Display::startManualScroller(effect, msg);
    idleOwnsScroller = Display::scrollerActive();
    return true;
  }

  bool advanceLegacyCounters() {
    int8_t n = nextEnabled(Counters::activeIndex());
    if (n < 0) return false;
    stopIdleScroller();
    Counters::setActiveIndex(static_cast<uint8_t>(n), false);
    Display::idleShowNext(Counters::getConst(n).count, Counters::idleEffect());
    return true;
  }

  bool advanceOnce() {
    const uint8_t len = Counters::idlePlaylistLen();
    if (len == 0) {
      return advanceLegacyCounters();
    }

    int8_t idx = nextValidPlaylistIndex(playlistCursor);
    if (idx < 0) return false;
    if (!showPlaylistStep(static_cast<uint8_t>(idx))) return false;
    playlistCursor = static_cast<uint8_t>((static_cast<uint8_t>(idx) + 1) % len);
    return true;
  }
}

namespace IdleCycle {

void begin() {
  lastActivityMs = millis();
  lastCycleMs = millis();
  cycling = false;
  idleOwnsScroller = false;
  playlistCursor = 0;
}

void noteActivity() {
  lastActivityMs = millis();
  if (cycling) {
    cycling = false;
    stopIdleScroller();
    Serial.println(F("[idle] activity — stop cycle"));
  }
}

bool isCycling() { return cycling; }

void loop() {
  // Manual (remote/web) scroller blocks idle; idle-owned marquee does not.
  if (Display::scrollerActive() && !idleOwnsScroller) {
    return;
  }

  const uint8_t plLen = Counters::idlePlaylistLen();
  if (plLen == 0 && enabledTotal() < 2) {
    cycling = false;
    return;
  }
  if (plLen > 0 && nextValidPlaylistIndex(0) < 0) {
    cycling = false;
    return;
  }

  // Don't cycle while blanked fully off
  if (Counters::blanked() && Counters::blankBrightnessPercent() == 0) {
    return;
  }

  const uint32_t now = millis();

  if (!cycling) {
    if (now - lastActivityMs >= (uint32_t)Counters::idleTimeoutSeconds() * 1000UL) {
      cycling = true;
      lastCycleMs = now;
      playlistCursor = 0;
      Serial.println(F("[idle] start cycle"));
      if (!advanceOnce()) {
        cycling = false;
      }
    }
    return;
  }

  if (now - lastCycleMs >= (uint32_t)Counters::idleCycleSeconds() * 1000UL) {
    lastCycleMs = now;
    if (!advanceOnce()) {
      cycling = false;
      stopIdleScroller();
    }
  }
}

}  // namespace IdleCycle
