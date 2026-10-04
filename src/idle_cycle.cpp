#include "idle_cycle.h"
#include "config.h"
#include "counters.h"
#include "display.h"

namespace {
  uint32_t lastActivityMs = 0;
  uint32_t lastCycleMs = 0;
  bool cycling = false;
  bool idleOwnsScroller = false;
  bool waitScrollerPass = false;  // hold until marquee finishes one pass (if scrolling)
  uint8_t playlistCursor = 0;  // next step to show
  uint8_t msgCursor = 0;       // next auto message index
  uint8_t cntCursor = 0;       // next auto counter index
  uint8_t prevOut = 0;         // effectOut of the step currently on screen

  int8_t nextEnabled(uint8_t from) {
    for (uint8_t step = 1; step <= MAX_COUNTERS; step++) {
      uint8_t i = (from + step) % MAX_COUNTERS;
      if (Counters::isEnabled(i)) return static_cast<int8_t>(i);
    }
    return -1;
  }

  int8_t nextEnabledFrom(uint8_t from) {
    for (uint8_t step = 0; step < MAX_COUNTERS; step++) {
      uint8_t i = (from + step) % MAX_COUNTERS;
      if (Counters::isEnabled(i)) return static_cast<int8_t>(i);
    }
    return -1;
  }

  int8_t nextMessageFrom(uint8_t from) {
    const uint8_t n = Counters::scrollMessageCount();
    if (n == 0) return -1;
    for (uint8_t step = 0; step < n; step++) {
      uint8_t i = (from + step) % n;
      const char* msg = Counters::scrollMessage(i);
      if (msg && msg[0]) return static_cast<int8_t>(i);
    }
    return -1;
  }

  uint8_t enabledTotal() {
    return Counters::enabledCount();
  }

  bool anyNonEmptyMessage() {
    return nextMessageFrom(0) >= 0;
  }

  bool stepIsValid(const IdleStep& step) {
    if (step.kind == IdleCounter) {
      if (step.index == IdleAutoIndex) return enabledTotal() > 0;
      return Counters::isEnabled(step.index);
    }
    if (step.kind == IdleMessage) {
      if (step.index == IdleAutoIndex) return anyNonEmptyMessage();
      if (step.index >= Counters::scrollMessageCount()) return false;
      const char* msg = Counters::scrollMessage(step.index);
      return msg && msg[0];
    }
    return false;
  }

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
      waitScrollerPass = false;
      if (Display::scrollerActive()) {
        Display::stopManualScroller(false);
      }
    }
  }

  // out(prev) → in(next); caller stamps hold after this returns.
  bool showPlaylistStep(uint8_t idx) {
    IdleStep step = Counters::idleStep(idx);
    if (!stepIsValid(step)) return false;
    const uint8_t effectIn = step.effectIn;
    const uint8_t effectOut = step.effectOut;
    const uint8_t exitEff = prevOut;

    if (step.kind == IdleCounter) {
      uint8_t ci = step.index;
      if (ci == IdleAutoIndex) {
        int8_t n = nextEnabledFrom(cntCursor);
        if (n < 0) return false;
        ci = static_cast<uint8_t>(n);
        cntCursor = static_cast<uint8_t>((ci + 1) % MAX_COUNTERS);
      }
      // Don't hard-clear — snapshot must see current pixels for the out anim.
      if (idleOwnsScroller) {
        idleOwnsScroller = false;
        waitScrollerPass = false;
      }
      Display::animateDisplayedOff(exitEff);
      Counters::setActiveIndex(ci, false);
      Display::idleShowNext(Counters::getConst(ci).count, effectIn);
      prevOut = effectOut;
      return true;
    }

    // Message: startManualScroller snapshots + exit-animates with prevOut.
    uint8_t mi = step.index;
    if (mi == IdleAutoIndex) {
      int8_t n = nextMessageFrom(msgCursor);
      if (n < 0) return false;
      mi = static_cast<uint8_t>(n);
      const uint8_t msgN = Counters::scrollMessageCount();
      msgCursor = msgN ? static_cast<uint8_t>((mi + 1) % msgN) : 0;
    }
    idleOwnsScroller = false;
    waitScrollerPass = false;
    const char* msg = Counters::scrollMessage(mi);
    Display::startManualScroller(effectIn, msg, Counters::scrollMessageBottom(mi), exitEff);
    idleOwnsScroller = Display::scrollerActive();
    // Only wait for a full pass when text actually scrolls (fit-only).
    waitScrollerPass = idleOwnsScroller && !Display::scrollerCompletedPass();
    prevOut = effectOut;
    return true;
  }

  bool advanceLegacyCounters() {
    int8_t n = nextEnabled(Counters::activeIndex());
    if (n < 0) return false;
    if (idleOwnsScroller) {
      idleOwnsScroller = false;
      waitScrollerPass = false;
    }
    Display::animateDisplayedOff(prevOut);
    Counters::setActiveIndex(static_cast<uint8_t>(n), false);
    Display::idleShowNext(Counters::getConst(n).count, Counters::idleEffectIn());
    prevOut = Counters::idleEffectOut();
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

  bool holdElapsed(uint32_t now) {
    // Scrolling messages: advance as soon as one full marquee pass finishes.
    if (waitScrollerPass) {
      if (Display::scrollerActive() && !Display::scrollerCompletedPass()) return false;
      waitScrollerPass = false;
      return true;
    }
    // Counters / static (fit) messages: hold for idle cycle seconds.
    const uint32_t holdMs = (uint32_t)Counters::idleCycleSeconds() * 1000UL;
    return (now - lastCycleMs) >= holdMs;
  }
}

namespace IdleCycle {

void begin() {
  lastActivityMs = millis();
  lastCycleMs = millis();
  cycling = false;
  idleOwnsScroller = false;
  waitScrollerPass = false;
  playlistCursor = 0;
  msgCursor = 0;
  cntCursor = 0;
  prevOut = Counters::idleEffectOut();
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

  if (Counters::blanked() && Counters::blankBrightnessPercent() == 0) {
    return;
  }

  const uint32_t now = millis();

  if (!cycling) {
    if (now - lastActivityMs >= (uint32_t)Counters::idleTimeoutSeconds() * 1000UL) {
      cycling = true;
      playlistCursor = 0;
      msgCursor = 0;
      cntCursor = 0;
      waitScrollerPass = false;
      prevOut = Counters::idleEffectOut();
      Serial.println(F("[idle] start cycle"));
      if (!advanceOnce()) {
        cycling = false;
      } else {
        // Hold starts after enter finishes (advanceOnce is blocking on anims).
        lastCycleMs = millis();
      }
    }
    return;
  }

  if (!holdElapsed(now)) return;

  if (!advanceOnce()) {
    cycling = false;
    stopIdleScroller();
  } else {
    lastCycleMs = millis();
  }
}

}  // namespace IdleCycle
