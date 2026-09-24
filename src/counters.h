#pragma once

#include "config.h"
#include <Arduino.h>

struct Counter {
  bool enabled;
  char name[NAME_MAX_LEN + 1];
  int32_t count;
};

namespace Counters {
  void begin();
  void load();
  void save();
  void markDirty();
  void saveIfDirty();

  uint8_t maxSlots();
  uint8_t enabledCount();
  Counter& get(uint8_t index);
  const Counter& getConst(uint8_t index);

  bool isEnabled(uint8_t index);
  void setEnabled(uint8_t index, bool enabled);
  void setName(uint8_t index, const char* name);
  void setCount(uint8_t index, int32_t count);
  bool inc(uint8_t index);
  bool dec(uint8_t index);

  uint8_t activeIndex();
  void setActiveIndex(uint8_t index);
  void setActiveIndex(uint8_t index, bool persist);  // persist=false for idle cycle


  uint8_t intensity();
  void setIntensity(uint8_t value);

  bool blanked();
  void setBlanked(bool blanked);

  // 0 = hardware display off when blanked; 1–100 = dim to that % of full brightness
  uint8_t blankBrightnessPercent();
  void setBlankBrightnessPercent(uint8_t percent);

  // Linked WiZ remote MAC as 12 hex chars (no separators), empty = accept any
  const char* linkedRemoteMac();
  void setLinkedRemoteMac(const char* mac12hex);
  void clearLinkedRemoteMac();

  // Global ShowMode ids (see show_mode.h) for non-keymap paths
  uint8_t idleEffect();
  void setIdleEffect(uint8_t effect);
  uint8_t localIncEffect();
  void setLocalIncEffect(uint8_t effect);
  uint8_t localDecEffect();
  void setLocalDecEffect(uint8_t effect);

  // Seconds between idle counter swaps (1–120)
  uint8_t idleCycleSeconds();
  void setIdleCycleSeconds(uint8_t seconds);

  // Transition / effect scroll speed 1 (slow) – 10 (fast)
  uint8_t scrollSpeed();
  void setScrollSpeed(uint8_t speed);

  // Name teeter bounce speed 1 (slow) – 10 (fast)
  uint8_t teeterSpeed();
  void setTeeterSpeed(uint8_t speed);

  // Manual scroller marquee text (persisted)
  const char* scrollMessage();
  void setScrollMessage(const char* text);
}
