#pragma once

#include "config.h"
#include <Arduino.h>

struct Counter {
  bool enabled;
  char name[NAME_MAX_LEN + 1];
  int32_t count;
};

enum IdleKind : uint8_t { IdleCounter = 0, IdleMessage = 1 };

// index == IdleAutoIndex → advance through messages/counters in list order
static constexpr uint8_t IdleAutoIndex = 255;

struct IdleStep {
  uint8_t kind;       // IdleKind
  uint8_t index;      // counter/message index, or IdleAutoIndex
  uint8_t effectIn;   // ShowMode enter
  uint8_t effectOut;  // ShowMode exit
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

  // 0 = full off (shutdown), 1–15 = MAX7219 intensity while blanked
  uint8_t blankBrightnessPercent();
  void setBlankBrightnessPercent(uint8_t percent);

  const char* linkedRemoteMac();
  void setLinkedRemoteMac(const char* mac12hex);
  void clearLinkedRemoteMac();

  // Global ShowMode ids for idle / local buttons
  uint8_t idleEffectIn();
  void setIdleEffectIn(uint8_t effect);
  uint8_t idleEffectOut();
  void setIdleEffectOut(uint8_t effect);
  // Legacy alias → idleEffectIn
  uint8_t idleEffect();
  void setIdleEffect(uint8_t effect);

  uint8_t localIncEffect();
  void setLocalIncEffect(uint8_t effect);
  uint8_t localDecEffect();
  void setLocalDecEffect(uint8_t effect);

  uint8_t idleCycleSeconds();
  void setIdleCycleSeconds(uint8_t seconds);

  uint16_t idleTimeoutSeconds();
  void setIdleTimeoutSeconds(uint16_t seconds);

  uint8_t scrollSpeed();
  void setScrollSpeed(uint8_t speed);

  // Per-ShowMode transition speed (1–20). RANDOM uses the resolved mode's speed.
  uint8_t effectSpeed(uint8_t showMode);
  void setEffectSpeed(uint8_t showMode, uint8_t speed);
  void setEffectSpeeds(const uint8_t* speeds, uint8_t count);

  uint8_t teeterSpeed();
  void setTeeterSpeed(uint8_t speed);

  uint8_t scrollMessageCount();
  const char* scrollMessage();
  const char* scrollMessage(uint8_t index);
  const char* scrollMessageBottom(uint8_t index);
  void setScrollMessage(const char* text);
  void setScrollMessages(const char* const* tops, const char* const* bottoms, uint8_t count);

  uint8_t idlePlaylistLen();
  IdleStep idleStep(uint8_t index);
  void setIdlePlaylist(const IdleStep* steps, uint8_t count);
}
