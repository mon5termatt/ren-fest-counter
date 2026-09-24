#include "remote_map.h"
#include "config.h"
#include "counters.h"
#include "display.h"
#include "idle_cycle.h"
#include "show_mode.h"
#include <Preferences.h>
#include <cstring>

namespace {
  Preferences prefs;

  constexpr uint8_t WIZ_ON = 1;
  constexpr uint8_t WIZ_OFF = 2;
  constexpr uint8_t WIZ_NIGHT = 3;
  constexpr uint8_t WIZ_BRIGHT_DOWN = 8;
  constexpr uint8_t WIZ_BRIGHT_UP = 9;
  constexpr uint8_t WIZ_ONE = 16;
  constexpr uint8_t WIZ_TWO = 17;
  constexpr uint8_t WIZ_THREE = 18;
  constexpr uint8_t WIZ_FOUR = 19;

  constexpr uint8_t KNOWN_BUTTONS[] = {
    WIZ_ON, WIZ_OFF, WIZ_NIGHT,
    WIZ_BRIGHT_DOWN, WIZ_BRIGHT_UP,
    WIZ_ONE, WIZ_TWO, WIZ_THREE, WIZ_FOUR
  };
  constexpr uint8_t KNOWN_COUNT = sizeof(KNOWN_BUTTONS) / sizeof(KNOWN_BUTTONS[0]);

  RemoteMap::Binding bindings[KNOWN_COUNT];

  int16_t pendingBtn = -1;
  uint32_t pendingMs = 0;
  bool pendingArmed = false;

  int findIndex(uint8_t buttonId) {
    for (uint8_t i = 0; i < KNOWN_COUNT; i++) {
      if (bindings[i].buttonId == buttonId) return i;
    }
    return -1;
  }

  uint8_t clampEffect(uint8_t e) {
    return ShowMode::valid(e) ? e : ShowMode::LEFT;
  }

  void setDefault(uint8_t id, uint8_t act, uint8_t param,
                  uint8_t effect,
                  uint8_t act2 = RemoteMap::ACTION_NONE, uint8_t param2 = 0,
                  uint8_t effect2 = ShowMode::LEFT) {
    for (uint8_t i = 0; i < KNOWN_COUNT; i++) {
      if (bindings[i].buttonId == id) {
        bindings[i].action = act;
        bindings[i].param = param;
        bindings[i].action2 = act2;
        bindings[i].param2 = param2;
        bindings[i].effect = effect;
        bindings[i].effect2 = effect2;
        return;
      }
    }
  }

  void applyDefaults() {
    for (uint8_t i = 0; i < KNOWN_COUNT; i++) {
      bindings[i].buttonId = KNOWN_BUTTONS[i];
      bindings[i].action = RemoteMap::ACTION_NONE;
      bindings[i].param = 0;
      bindings[i].action2 = RemoteMap::ACTION_NONE;
      bindings[i].param2 = 0;
      bindings[i].effect = ShowMode::LEFT;
      bindings[i].effect2 = ShowMode::LEFT;
    }

    setDefault(WIZ_ONE, RemoteMap::ACTION_SELECT, 0, ShowMode::LEFT);
    setDefault(WIZ_TWO, RemoteMap::ACTION_SELECT, 1, ShowMode::LEFT);
    setDefault(WIZ_THREE, RemoteMap::ACTION_SELECT, 2, ShowMode::LEFT);
    setDefault(WIZ_FOUR, RemoteMap::ACTION_SELECT, 3, ShowMode::LEFT);
    setDefault(WIZ_BRIGHT_UP, RemoteMap::ACTION_INC_ACTIVE, 0, ShowMode::UP);
    setDefault(WIZ_BRIGHT_DOWN, RemoteMap::ACTION_DEC_ACTIVE, 0, ShowMode::DOWN);
    setDefault(WIZ_ON, RemoteMap::ACTION_UNBLANK, 0, ShowMode::FREEZE);
    setDefault(WIZ_OFF, RemoteMap::ACTION_BLANK, 0, ShowMode::FREEZE);
    setDefault(WIZ_NIGHT, RemoteMap::ACTION_NONE, 0, ShowMode::FREEZE);
  }

  void execute(uint8_t action, uint8_t param, uint8_t effect) {
    const uint8_t eff = clampEffect(effect);

    if (action != RemoteMap::ACTION_SCROLLER && Display::scrollerActive()) {
      // Select: halt marquee (blank slate); selectShow plays the key's enter transition.
      // Other actions restore the scoreboard immediately.
      Display::stopManualScroller(action != RemoteMap::ACTION_SELECT);
    }

    switch (action) {
      case RemoteMap::ACTION_NONE:
        break;

      case RemoteMap::ACTION_SELECT:
        if (Counters::isEnabled(param)) {
          Counters::setActiveIndex(param);
          // Enter transition for this counter (Anim on the binding).
          Display::selectShow(eff);
          Counters::saveIfDirty();
          Serial.printf("[wiz] select counter %u (%s) anim %u\n", param + 1,
                        Counters::getConst(param).name, eff);
        } else {
          Serial.printf("[wiz] select %u ignored (disabled)\n", param + 1);
        }
        break;

      case RemoteMap::ACTION_INC_ACTIVE: {
        uint8_t a = Counters::activeIndex();
        if (Counters::inc(a)) {
          Display::onCountIncreased(Counters::getConst(a).count, eff);
          Counters::saveIfDirty();
        }
        break;
      }

      case RemoteMap::ACTION_DEC_ACTIVE: {
        uint8_t a = Counters::activeIndex();
        if (Counters::dec(a)) {
          Display::onCountDecreased(Counters::getConst(a).count, eff);
          Counters::saveIfDirty();
        }
        break;
      }

      case RemoteMap::ACTION_BLANK:
        Counters::setBlanked(true);
        Display::applyBlanked();
        Counters::saveIfDirty();
        break;

      case RemoteMap::ACTION_UNBLANK:
        Counters::setBlanked(false);
        Display::applyBlanked();
        Counters::saveIfDirty();
        break;

      case RemoteMap::ACTION_SCROLLER:
        IdleCycle::noteActivity();
        if (Display::scrollerActive()) {
          Display::stopManualScroller();
          Serial.println(F("[wiz] scroller stop"));
        } else {
          Display::startManualScroller(eff);
          Serial.println(F("[wiz] scroller start"));
        }
        break;

      default:
        break;
    }
  }

  void flushPendingTap() {
    if (!pendingArmed) return;
    int idx = findIndex(static_cast<uint8_t>(pendingBtn));
    pendingArmed = false;
    if (idx < 0) return;
    execute(bindings[idx].action, bindings[idx].param, bindings[idx].effect);
  }
}

namespace RemoteMap {

void factoryDefaults() {
  applyDefaults();
}

void begin() {
  applyDefaults();
  load();
  pendingArmed = false;
}

void loop() {
  if (pendingArmed && (millis() - pendingMs) >= DOUBLE_TAP_MS) {
    flushPendingTap();
  }
}

void load() {
  if (!prefs.begin("rfremote", true)) return;

  for (uint8_t i = 0; i < KNOWN_COUNT; i++) {
    char keyA[8], keyP[8], keyA2[8], keyP2[8], keyE[8], keyE2[8];
    snprintf(keyA, sizeof(keyA), "a%u", bindings[i].buttonId);
    snprintf(keyP, sizeof(keyP), "p%u", bindings[i].buttonId);
    snprintf(keyA2, sizeof(keyA2), "A%u", bindings[i].buttonId);
    snprintf(keyP2, sizeof(keyP2), "P%u", bindings[i].buttonId);
    snprintf(keyE, sizeof(keyE), "e%u", bindings[i].buttonId);
    snprintf(keyE2, sizeof(keyE2), "E%u", bindings[i].buttonId);
    bindings[i].action = prefs.getUChar(keyA, bindings[i].action);
    bindings[i].param = prefs.getUChar(keyP, bindings[i].param);
    bindings[i].action2 = prefs.getUChar(keyA2, bindings[i].action2);
    bindings[i].param2 = prefs.getUChar(keyP2, bindings[i].param2);
    bindings[i].effect = clampEffect(prefs.getUChar(keyE, bindings[i].effect));
    bindings[i].effect2 = clampEffect(prefs.getUChar(keyE2, bindings[i].effect2));
  }

  prefs.end();
}

void save() {
  if (!prefs.begin("rfremote", false)) return;

  for (uint8_t i = 0; i < KNOWN_COUNT; i++) {
    char keyA[8], keyP[8], keyA2[8], keyP2[8], keyE[8], keyE2[8];
    snprintf(keyA, sizeof(keyA), "a%u", bindings[i].buttonId);
    snprintf(keyP, sizeof(keyP), "p%u", bindings[i].buttonId);
    snprintf(keyA2, sizeof(keyA2), "A%u", bindings[i].buttonId);
    snprintf(keyP2, sizeof(keyP2), "P%u", bindings[i].buttonId);
    snprintf(keyE, sizeof(keyE), "e%u", bindings[i].buttonId);
    snprintf(keyE2, sizeof(keyE2), "E%u", bindings[i].buttonId);
    prefs.putUChar(keyA, bindings[i].action);
    prefs.putUChar(keyP, bindings[i].param);
    prefs.putUChar(keyA2, bindings[i].action2);
    prefs.putUChar(keyP2, bindings[i].param2);
    prefs.putUChar(keyE, bindings[i].effect);
    prefs.putUChar(keyE2, bindings[i].effect2);
  }

  prefs.end();
}

void handleButton(uint8_t buttonId) {
  int idx = findIndex(buttonId);
  if (idx < 0) return;

  IdleCycle::noteActivity();

  const Binding& b = bindings[idx];
  const uint32_t now = millis();

  if (pendingArmed && pendingBtn == static_cast<int16_t>(buttonId) &&
      (now - pendingMs) < DOUBLE_TAP_MS) {
    pendingArmed = false;
    Serial.printf("[wiz] double-tap btn %u\n", buttonId);
    execute(b.action2, b.param2, b.effect2);
    return;
  }

  if (pendingArmed) {
    flushPendingTap();
  }

  if (b.action2 != ACTION_NONE) {
    pendingBtn = buttonId;
    pendingMs = now;
    pendingArmed = true;
    return;
  }

  execute(b.action, b.param, b.effect);
}

uint8_t bindingCount() { return KNOWN_COUNT; }

Binding getBinding(uint8_t index) {
  if (index >= KNOWN_COUNT) {
    return Binding{0, ACTION_NONE, 0, ACTION_NONE, 0, ShowMode::LEFT, ShowMode::LEFT};
  }
  return bindings[index];
}

bool setBinding(uint8_t buttonId, uint8_t action, uint8_t param,
                uint8_t action2, uint8_t param2,
                uint8_t effect, uint8_t effect2) {
  int idx = findIndex(buttonId);
  if (idx < 0) return false;
  if (action > ACTION_SCROLLER || action2 > ACTION_SCROLLER) return false;
  if (action == ACTION_SELECT && param >= MAX_COUNTERS) return false;
  if (action2 == ACTION_SELECT && param2 >= MAX_COUNTERS) return false;
  bindings[idx].action = action;
  bindings[idx].param = param;
  bindings[idx].action2 = action2;
  bindings[idx].param2 = param2;
  bindings[idx].effect = clampEffect(effect);
  bindings[idx].effect2 = clampEffect(effect2);
  return true;
}

const uint8_t* knownButtons(uint8_t& count) {
  count = KNOWN_COUNT;
  return KNOWN_BUTTONS;
}

const char* buttonLabel(uint8_t buttonId) {
  switch (buttonId) {
    case WIZ_ON: return "ON";
    case WIZ_OFF: return "OFF";
    case WIZ_NIGHT: return "NIGHT";
    case WIZ_BRIGHT_DOWN: return "Bright -";
    case WIZ_BRIGHT_UP: return "Bright +";
    case WIZ_ONE: return "1";
    case WIZ_TWO: return "2";
    case WIZ_THREE: return "3";
    case WIZ_FOUR: return "4";
    default: return "?";
  }
}

const char* actionLabel(uint8_t action) {
  switch (action) {
    case ACTION_NONE: return "none";
    case ACTION_SELECT: return "select";
    case ACTION_INC_ACTIVE: return "inc_active";
    case ACTION_DEC_ACTIVE: return "dec_active";
    case ACTION_BLANK: return "blank";
    case ACTION_UNBLANK: return "unblank";
    case ACTION_SCROLLER: return "scroller";
    default: return "?";
  }
}

}  // namespace RemoteMap
