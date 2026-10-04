#pragma once

#include "config.h"
#include <Arduino.h>

namespace Display {
  void begin();
  void loop();
  void refreshAll();
  void refresh(uint8_t index);   // single-display: refreshes the shared panel
  void applyIntensity();
  void applyBlanked();
  void flashActive(uint8_t index);  // flash shared panel (selection cue)

  // effect = ShowMode id; speed 0 = use per-anim default (1–20)
  void onCountIncreased(int32_t newCount, uint8_t effect = 2, uint8_t speed = 0);
  void onCountDecreased(int32_t newCount, uint8_t effect = 3, uint8_t speed = 0);
  void idleShowNext(int32_t count, uint8_t effect = 0, uint8_t speed = 0);
  void selectShow(uint8_t effect = 0, uint8_t speed = 0);

  // Manual marquee. nullptr text = cycle all web-UI scrollMessages until stop.
  // Explicit text = one-shot (idle playlist). effectIn = marquee dir;
  // effectOut = exit of previous on-screen content. speed 0 = per-anim default.
  void startManualScroller(uint8_t effectIn = 0, const char* text = nullptr,
                           const char* bottom = nullptr, uint8_t effectOut = 0,
                           uint8_t speed = 0);
  // restore=true paints the scoreboard; false clears and leaves the next anim to fill
  void stopManualScroller(bool restore = true);
  bool scrollerActive();
  // True after the current marquee has finished at least one full pass
  bool scrollerCompletedPass();
  // Animate whatever is on-screen off using ShowMode (for idle transitions)
  bool animateDisplayedOff(uint8_t effect, uint8_t speed = 0);

  uint8_t primaryBoard();
}
