#include "counters.h"
#include "show_mode.h"
#include <Preferences.h>
#include <cstring>

namespace {
  Preferences prefs;
  Counter counters[MAX_COUNTERS];
  uint8_t activeIdx = 0;
  uint8_t ledIntensity = DEFAULT_INTENSITY;
  bool displaysBlanked = false;
  uint8_t blankBrightPct = 0;  // 0 = full off, 1–100 = dim %
  char linkedMac[13] = "";
  uint8_t idleEff = ShowMode::LEFT;
  uint8_t localIncEff = ShowMode::UP;
  uint8_t localDecEff = ShowMode::DOWN;
  uint8_t idleCycleSec = 5;
  uint16_t idleTimeoutSec = 30;  // seconds before attract cycle starts
  uint8_t scrollSpd = 5;
  uint8_t teeterSpd = 5;
  char scrollMessages[MAX_SCROLL_MESSAGES][SCROLLER_MAX_LEN + 1];
  uint8_t scrollMsgCount = 1;
  IdleStep idlePlaylist[MAX_IDLE_PLAYLIST];
  uint8_t idlePlLen = 0;
  bool dirty = false;

  uint8_t clampEffect(uint8_t e) {
    return ShowMode::valid(e) ? e : ShowMode::LEFT;
  }

  uint8_t clampCycleSec(uint8_t s) {
    if (s < 1) return 1;
    if (s > 120) return 120;
    return s;
  }

  uint16_t clampTimeoutSec(uint16_t s) {
    if (s < 5) return 5;
    if (s > 600) return 600;
    return s;
  }

  uint8_t clampScroll(uint8_t s) {
    if (s < 1) return 1;
    if (s > 10) return 10;
    return s;
  }

  uint8_t clampMsgCount(uint8_t n) {
    if (n < 1) return 1;
    if (n > MAX_SCROLL_MESSAGES) return MAX_SCROLL_MESSAGES;
    return n;
  }

  void clampCount(int32_t& c) {
    if (c < 0) c = 0;
    if (c > COUNT_MAX) c = COUNT_MAX;
  }

  void clearMessages() {
    for (uint8_t i = 0; i < MAX_SCROLL_MESSAGES; i++) {
      scrollMessages[i][0] = '\0';
    }
    scrollMsgCount = 1;
  }

  void defaults() {
    for (uint8_t i = 0; i < MAX_COUNTERS; i++) {
      counters[i].enabled = (i < DEFAULT_ENABLED_COUNT);
      counters[i].count = 0;
      snprintf(counters[i].name, sizeof(counters[i].name), "CNT%d", i + 1);
    }
    activeIdx = 0;
    ledIntensity = DEFAULT_INTENSITY;
    displaysBlanked = false;
    blankBrightPct = 0;
    linkedMac[0] = '\0';
    idleEff = ShowMode::LEFT;
    localIncEff = ShowMode::UP;
    localDecEff = ShowMode::DOWN;
    idleCycleSec = 5;
    idleTimeoutSec = 30;
    scrollSpd = 5;
    teeterSpd = 5;
    clearMessages();
    idlePlLen = 0;
  }
}

namespace Counters {

void begin() {
  defaults();
  load();
}

void load() {
  if (!prefs.begin("rfcounters", true)) {
    return;
  }

  ledIntensity = prefs.getUChar("intensity", DEFAULT_INTENSITY);
  if (ledIntensity > 15) ledIntensity = 15;
  displaysBlanked = prefs.getBool("blanked", false);
  blankBrightPct = prefs.getUChar("blankPct", 0);
  if (blankBrightPct > 100) blankBrightPct = 100;
  activeIdx = prefs.getUChar("active", 0);
  if (activeIdx >= MAX_COUNTERS) activeIdx = 0;

  String mac = prefs.getString("linkmac", "");
  if (mac.length() == 12) {
    strncpy(linkedMac, mac.c_str(), 12);
    linkedMac[12] = '\0';
  } else {
    linkedMac[0] = '\0';
  }

  idleEff = clampEffect(prefs.getUChar("idleEff", ShowMode::LEFT));
  localIncEff = clampEffect(prefs.getUChar("locIncEff", ShowMode::UP));
  localDecEff = clampEffect(prefs.getUChar("locDecEff", ShowMode::DOWN));
  idleCycleSec = clampCycleSec(prefs.getUChar("idleSec", 5));
  idleTimeoutSec = clampTimeoutSec(prefs.getUShort("idleTo", 30));
  scrollSpd = clampScroll(prefs.getUChar("scrollSpd", 5));
  teeterSpd = clampScroll(prefs.getUChar("teeterSpd", 5));

  clearMessages();
  if (prefs.isKey("msgCnt")) {
    scrollMsgCount = clampMsgCount(prefs.getUChar("msgCnt", 1));
    for (uint8_t i = 0; i < scrollMsgCount; i++) {
      char key[8];
      snprintf(key, sizeof(key), "msg%u", i);
      String sm = prefs.getString(key, "");
      if (sm.length() > 0 && sm.length() <= SCROLLER_MAX_LEN) {
        strncpy(scrollMessages[i], sm.c_str(), SCROLLER_MAX_LEN);
        scrollMessages[i][SCROLLER_MAX_LEN] = '\0';
      }
    }
  } else {
    // Legacy single scrollMsg → message 0
    String sm = prefs.getString("scrollMsg", "");
    if (sm.length() > 0 && sm.length() <= SCROLLER_MAX_LEN) {
      strncpy(scrollMessages[0], sm.c_str(), SCROLLER_MAX_LEN);
      scrollMessages[0][SCROLLER_MAX_LEN] = '\0';
    }
    scrollMsgCount = 1;
  }

  idlePlLen = 0;
  if (prefs.isKey("plLen")) {
    uint8_t n = prefs.getUChar("plLen", 0);
    if (n > MAX_IDLE_PLAYLIST) n = MAX_IDLE_PLAYLIST;
    for (uint8_t i = 0; i < n; i++) {
      char key[8];
      snprintf(key, sizeof(key), "pl%u", i);
      uint16_t packed = prefs.getUShort(key, 0xFFFF);
      if (packed == 0xFFFF) continue;
      IdleStep step;
      step.kind = static_cast<uint8_t>((packed >> 8) & 0xFF);
      step.index = static_cast<uint8_t>(packed & 0xFF);
      char ek[8];
      snprintf(ek, sizeof(ek), "pe%u", i);
      step.effect = clampEffect(prefs.getUChar(ek, idleEff));
      if (step.kind > IdleMessage) continue;
      if (step.kind == IdleCounter && step.index >= MAX_COUNTERS) continue;
      if (step.kind == IdleMessage && step.index >= MAX_SCROLL_MESSAGES) continue;
      idlePlaylist[idlePlLen++] = step;
    }
  }

  for (uint8_t i = 0; i < MAX_COUNTERS; i++) {
    char key[12];
    snprintf(key, sizeof(key), "en%u", i);
    counters[i].enabled = prefs.getBool(key, i < DEFAULT_ENABLED_COUNT);

    snprintf(key, sizeof(key), "cnt%u", i);
    counters[i].count = prefs.getInt(key, 0);
    clampCount(counters[i].count);

    snprintf(key, sizeof(key), "nm%u", i);
    String nm = prefs.getString(key, "");
    if (nm.length() > 0) {
      strncpy(counters[i].name, nm.c_str(), NAME_MAX_LEN);
      counters[i].name[NAME_MAX_LEN] = '\0';
    }
  }

  prefs.end();
  dirty = false;
}

void save() {
  if (!prefs.begin("rfcounters", false)) {
    return;
  }

  prefs.putUChar("intensity", ledIntensity);
  prefs.putBool("blanked", displaysBlanked);
  prefs.putUChar("blankPct", blankBrightPct);
  prefs.putUChar("active", activeIdx);
  prefs.putString("linkmac", linkedMac);
  prefs.putUChar("idleEff", idleEff);
  prefs.putUChar("locIncEff", localIncEff);
  prefs.putUChar("locDecEff", localDecEff);
  prefs.putUChar("idleSec", idleCycleSec);
  prefs.putUShort("idleTo", idleTimeoutSec);
  prefs.putUChar("scrollSpd", scrollSpd);
  prefs.putUChar("teeterSpd", teeterSpd);
  prefs.putUChar("msgCnt", scrollMsgCount);
  for (uint8_t i = 0; i < MAX_SCROLL_MESSAGES; i++) {
    char key[8];
    snprintf(key, sizeof(key), "msg%u", i);
    if (i < scrollMsgCount) {
      prefs.putString(key, scrollMessages[i]);
    } else {
      prefs.remove(key);
    }
  }
  // Keep legacy key in sync for older tooling
  prefs.putString("scrollMsg", scrollMessages[0]);

  prefs.putUChar("plLen", idlePlLen);
  for (uint8_t i = 0; i < MAX_IDLE_PLAYLIST; i++) {
    char key[8];
    snprintf(key, sizeof(key), "pl%u", i);
    char ek[8];
    snprintf(ek, sizeof(ek), "pe%u", i);
    if (i < idlePlLen) {
      uint16_t packed = static_cast<uint16_t>((idlePlaylist[i].kind << 8) | idlePlaylist[i].index);
      prefs.putUShort(key, packed);
      prefs.putUChar(ek, idlePlaylist[i].effect);
    } else {
      prefs.remove(key);
      prefs.remove(ek);
    }
  }

  for (uint8_t i = 0; i < MAX_COUNTERS; i++) {
    char key[12];
    snprintf(key, sizeof(key), "en%u", i);
    prefs.putBool(key, counters[i].enabled);

    snprintf(key, sizeof(key), "cnt%u", i);
    prefs.putInt(key, counters[i].count);

    snprintf(key, sizeof(key), "nm%u", i);
    prefs.putString(key, counters[i].name);
  }

  prefs.end();
  dirty = false;
}

void markDirty() { dirty = true; }

void saveIfDirty() {
  if (dirty) save();
}

uint8_t maxSlots() { return MAX_COUNTERS; }

uint8_t enabledCount() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_COUNTERS; i++) {
    if (counters[i].enabled) n++;
  }
  return n;
}

Counter& get(uint8_t index) {
  return counters[index < MAX_COUNTERS ? index : 0];
}

const Counter& getConst(uint8_t index) {
  return counters[index < MAX_COUNTERS ? index : 0];
}

bool isEnabled(uint8_t index) {
  return index < MAX_COUNTERS && counters[index].enabled;
}

void setEnabled(uint8_t index, bool enabled) {
  if (index >= MAX_COUNTERS) return;
  // Logical counters don't need their own CS in SINGLE_DISPLAY mode
  if (!SINGLE_DISPLAY && CS_PINS[index] < 0) enabled = false;
  counters[index].enabled = enabled;
  if (!enabled && activeIdx == index) {
    for (uint8_t i = 0; i < MAX_COUNTERS; i++) {
      if (counters[i].enabled) {
        activeIdx = i;
        break;
      }
    }
  }
  markDirty();
}

void setName(uint8_t index, const char* name) {
  if (index >= MAX_COUNTERS || !name) return;
  strncpy(counters[index].name, name, NAME_MAX_LEN);
  counters[index].name[NAME_MAX_LEN] = '\0';
  markDirty();
}

void setCount(uint8_t index, int32_t count) {
  if (index >= MAX_COUNTERS) return;
  clampCount(count);
  counters[index].count = count;
  markDirty();
}

bool inc(uint8_t index) {
  if (!isEnabled(index)) return false;
  if (counters[index].count >= COUNT_MAX) return false;
  counters[index].count++;
  markDirty();
  return true;
}

bool dec(uint8_t index) {
  if (!isEnabled(index)) return false;
  if (counters[index].count <= 0) return false;
  counters[index].count--;
  markDirty();
  return true;
}

uint8_t activeIndex() { return activeIdx; }

void setActiveIndex(uint8_t index) {
  setActiveIndex(index, true);
}

void setActiveIndex(uint8_t index, bool persist) {
  if (index >= MAX_COUNTERS) return;
  if (!counters[index].enabled) return;
  if (activeIdx == index) {
    if (persist) markDirty();
    return;
  }
  activeIdx = index;
  if (persist) markDirty();
}

uint8_t intensity() { return ledIntensity; }

void setIntensity(uint8_t value) {
  if (value > 15) value = 15;
  ledIntensity = value;
  markDirty();
}

bool blanked() { return displaysBlanked; }

void setBlanked(bool blanked) {
  displaysBlanked = blanked;
  markDirty();
}

uint8_t blankBrightnessPercent() { return blankBrightPct; }

void setBlankBrightnessPercent(uint8_t percent) {
  if (percent > 100) percent = 100;
  blankBrightPct = percent;
  markDirty();
}

const char* linkedRemoteMac() { return linkedMac; }

void setLinkedRemoteMac(const char* mac12hex) {
  if (!mac12hex) {
    linkedMac[0] = '\0';
  } else {
    strncpy(linkedMac, mac12hex, 12);
    linkedMac[12] = '\0';
  }
  markDirty();
}

void clearLinkedRemoteMac() {
  linkedMac[0] = '\0';
  markDirty();
}

uint8_t idleEffect() { return idleEff; }

void setIdleEffect(uint8_t effect) {
  idleEff = clampEffect(effect);
  markDirty();
}

uint8_t localIncEffect() { return localIncEff; }

void setLocalIncEffect(uint8_t effect) {
  localIncEff = clampEffect(effect);
  markDirty();
}

uint8_t localDecEffect() { return localDecEff; }

void setLocalDecEffect(uint8_t effect) {
  localDecEff = clampEffect(effect);
  markDirty();
}

uint8_t idleCycleSeconds() { return idleCycleSec; }

void setIdleCycleSeconds(uint8_t seconds) {
  idleCycleSec = clampCycleSec(seconds);
  markDirty();
}

uint16_t idleTimeoutSeconds() { return idleTimeoutSec; }

void setIdleTimeoutSeconds(uint16_t seconds) {
  idleTimeoutSec = clampTimeoutSec(seconds);
  markDirty();
}

uint8_t scrollSpeed() { return scrollSpd; }

void setScrollSpeed(uint8_t speed) {
  scrollSpd = clampScroll(speed);
  markDirty();
}

uint8_t teeterSpeed() { return teeterSpd; }

void setTeeterSpeed(uint8_t speed) {
  teeterSpd = clampScroll(speed);
  markDirty();
}

const char* scrollMessage() { return scrollMessages[0]; }

const char* scrollMessage(uint8_t index) {
  if (index >= scrollMsgCount) return "";
  return scrollMessages[index];
}

uint8_t scrollMessageCount() { return scrollMsgCount; }

void setScrollMessage(const char* text) {
  if (!text) {
    scrollMessages[0][0] = '\0';
  } else {
    strncpy(scrollMessages[0], text, SCROLLER_MAX_LEN);
    scrollMessages[0][SCROLLER_MAX_LEN] = '\0';
  }
  if (scrollMsgCount < 1) scrollMsgCount = 1;
  markDirty();
}

void setScrollMessages(const char* const* texts, uint8_t count) {
  clearMessages();
  scrollMsgCount = clampMsgCount(count);
  if (!texts) {
    markDirty();
    return;
  }
  for (uint8_t i = 0; i < scrollMsgCount; i++) {
    if (texts[i]) {
      strncpy(scrollMessages[i], texts[i], SCROLLER_MAX_LEN);
      scrollMessages[i][SCROLLER_MAX_LEN] = '\0';
    }
  }
  markDirty();
}

uint8_t idlePlaylistLen() { return idlePlLen; }

IdleStep idleStep(uint8_t index) {
  if (index >= idlePlLen) {
    IdleStep empty = {IdleCounter, 0, ShowMode::LEFT};
    return empty;
  }
  return idlePlaylist[index];
}

void setIdlePlaylist(const IdleStep* steps, uint8_t count) {
  idlePlLen = 0;
  if (!steps || count == 0) {
    markDirty();
    return;
  }
  if (count > MAX_IDLE_PLAYLIST) count = MAX_IDLE_PLAYLIST;
  for (uint8_t i = 0; i < count; i++) {
    IdleStep step = steps[i];
    if (step.kind > IdleMessage) continue;
    if (step.kind == IdleCounter && step.index >= MAX_COUNTERS) continue;
    if (step.kind == IdleMessage && step.index >= MAX_SCROLL_MESSAGES) continue;
    step.effect = clampEffect(step.effect);
    idlePlaylist[idlePlLen++] = step;
  }
  markDirty();
}

}  // namespace Counters
