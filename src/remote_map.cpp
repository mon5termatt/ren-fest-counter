#include "remote_map.h"
#include "config.h"
#include "counters.h"
#include "display.h"
#include "idle_cycle.h"
#include "setup_menu.h"
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

  // ON×3 then OFF×3 (≤1.5s between taps) opens setup.
  // First ON unblanks immediately; further taps only advance the combo.
  constexpr uint8_t COMBO_NEED = 3;
  constexpr uint32_t COMBO_GAP_MS = 1500;
  uint8_t comboOnCount = 0;
  uint8_t comboOffCount = 0;
  uint32_t comboLastMs = 0;
  bool comboActive = false;
  bool comboOnFired = false;  // first ON already ran UNBLANK

  void clearComboArm() {
    comboOnCount = 0;
    comboOffCount = 0;
    comboActive = false;
    comboOnFired = false;
  }

  int findIndex(uint8_t buttonId);  // defined below

  void execute(uint8_t action, uint8_t param, uint8_t effectIn, uint8_t effectOut,
               uint8_t speed);  // defined below

  void fireBindingIndex(int idx) {
    if (idx < 0 || idx >= static_cast<int>(KNOWN_COUNT)) return;
    const RemoteMap::Binding& b = bindings[idx];
    if (b.action != RemoteMap::ACTION_IDLE_START) {
      IdleCycle::noteActivity();
    }
    execute(b.action, b.param, b.effectIn, b.effectOut, b.speed);
  }

  int findIndex(uint8_t buttonId) {
    for (uint8_t i = 0; i < KNOWN_COUNT; i++) {
      if (bindings[i].buttonId == buttonId) return i;
    }
    return -1;
  }

  uint8_t clampEffect(uint8_t e) {
    return ShowMode::valid(e) ? e : ShowMode::LEFT;
  }

  uint8_t clampSpeed(uint8_t s) {
    if (s == 0) return 0;  // 0 = use anim-type default
    if (s > 20) return 20;
    return s;
  }

  void setDefault(uint8_t id, uint8_t act, uint8_t param,
                  uint8_t effectIn, uint8_t effectOut = ShowMode::LEFT,
                  uint8_t speed = 0) {
    for (uint8_t i = 0; i < KNOWN_COUNT; i++) {
      if (bindings[i].buttonId == id) {
        bindings[i].action = act;
        bindings[i].param = param;
        bindings[i].effectIn = effectIn;
        bindings[i].effectOut = effectOut;
        bindings[i].speed = speed;
        return;
      }
    }
  }

  void applyDefaults() {
    for (uint8_t i = 0; i < KNOWN_COUNT; i++) {
      bindings[i].buttonId = KNOWN_BUTTONS[i];
      bindings[i].action = RemoteMap::ACTION_NONE;
      bindings[i].param = 0;
      bindings[i].effectIn = ShowMode::LEFT;
      bindings[i].effectOut = ShowMode::LEFT;
      bindings[i].speed = 0;
    }

    setDefault(WIZ_ONE, RemoteMap::ACTION_SELECT, 0, ShowMode::LEFT, ShowMode::LEFT);
    setDefault(WIZ_TWO, RemoteMap::ACTION_SELECT, 1, ShowMode::LEFT, ShowMode::LEFT);
    setDefault(WIZ_THREE, RemoteMap::ACTION_SELECT, 2, ShowMode::LEFT, ShowMode::LEFT);
    setDefault(WIZ_FOUR, RemoteMap::ACTION_SELECT, 3, ShowMode::LEFT, ShowMode::LEFT);
    setDefault(WIZ_BRIGHT_UP, RemoteMap::ACTION_INC_ACTIVE, 0, ShowMode::UP, ShowMode::LEFT);
    setDefault(WIZ_BRIGHT_DOWN, RemoteMap::ACTION_DEC_ACTIVE, 0, ShowMode::DOWN, ShowMode::LEFT);
    setDefault(WIZ_ON, RemoteMap::ACTION_UNBLANK, 0, ShowMode::FREEZE, ShowMode::FREEZE);
    setDefault(WIZ_OFF, RemoteMap::ACTION_BLANK, 0, ShowMode::FREEZE, ShowMode::FREEZE);
    setDefault(WIZ_NIGHT, RemoteMap::ACTION_SCROLLER, 0, ShowMode::LEFT, ShowMode::LEFT);
  }

  void execute(uint8_t action, uint8_t param, uint8_t effectIn, uint8_t effectOut,
               uint8_t speed) {
    const uint8_t inEff = clampEffect(effectIn);
    const uint8_t outEff = clampEffect(effectOut);
    const uint8_t spd = clampSpeed(speed);

    if (action != RemoteMap::ACTION_SCROLLER && Display::scrollerActive()) {
      // Animate marquee off with this key's out, then continue the action.
      Display::animateDisplayedOff(outEff, spd);
      Display::stopManualScroller(action != RemoteMap::ACTION_SELECT);
    }

    switch (action) {
      case RemoteMap::ACTION_NONE:
        break;

      case RemoteMap::ACTION_SELECT:
        if (Counters::isEnabled(param)) {
          Counters::setActiveIndex(param);
          Display::selectShow(inEff, spd);
          Counters::saveIfDirty();
          Serial.printf("[wiz] select counter %u (%s) in %u out %u spd %u\n", param + 1,
                        Counters::getConst(param).name, inEff, outEff, spd);
        } else {
          Serial.printf("[wiz] select %u ignored (disabled)\n", param + 1);
        }
        break;

      case RemoteMap::ACTION_INC_ACTIVE: {
        uint8_t a = Counters::activeIndex();
        if (Counters::inc(a)) {
          Display::onCountIncreased(Counters::getConst(a).count, inEff, spd);
          Counters::saveIfDirty();
        }
        break;
      }

      case RemoteMap::ACTION_DEC_ACTIVE: {
        uint8_t a = Counters::activeIndex();
        if (Counters::dec(a)) {
          Display::onCountDecreased(Counters::getConst(a).count, inEff, spd);
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
          Display::animateDisplayedOff(outEff, spd);
          Display::stopManualScroller();
          Serial.println(F("[wiz] scroller stop"));
        } else {
          Display::startManualScroller(inEff, nullptr, nullptr, outEff, spd);
          Serial.println(F("[wiz] scroller start"));
        }
        break;

      case RemoteMap::ACTION_IDLE_START:
        if (IdleCycle::isCycling()) {
          IdleCycle::noteActivity();
          Serial.println(F("[wiz] idle stop"));
        } else if (IdleCycle::startNow()) {
          Serial.println(F("[wiz] idle start"));
        } else {
          Serial.println(F("[wiz] idle start failed"));
        }
        break;

      default:
        break;
    }
  }
}

namespace RemoteMap {

void factoryDefaults() {
  applyDefaults();
}

void begin() {
  applyDefaults();
  load();
}

void loop() {
  if (!comboActive) return;
  if (static_cast<uint32_t>(millis() - comboLastMs) < COMBO_GAP_MS) return;
  // Sequence timed out — first ON already unblanked; just drop the arm.
  clearComboArm();
}

void load() {
  if (!prefs.begin("rfremote", true)) return;

  for (uint8_t i = 0; i < KNOWN_COUNT; i++) {
    char keyA[8], keyP[8], keyE[8], keyE2[8], keyS[8];
    snprintf(keyA, sizeof(keyA), "a%u", bindings[i].buttonId);
    snprintf(keyP, sizeof(keyP), "p%u", bindings[i].buttonId);
    snprintf(keyE, sizeof(keyE), "e%u", bindings[i].buttonId);
    snprintf(keyE2, sizeof(keyE2), "E%u", bindings[i].buttonId);
    snprintf(keyS, sizeof(keyS), "s%u", bindings[i].buttonId);
    bindings[i].action = prefs.getUChar(keyA, bindings[i].action);
    bindings[i].param = prefs.getUChar(keyP, bindings[i].param);
    // Legacy: e = effectIn, E = effectOut (was double-tap effect)
    bindings[i].effectIn = clampEffect(prefs.getUChar(keyE, bindings[i].effectIn));
    bindings[i].effectOut = clampEffect(prefs.getUChar(keyE2, bindings[i].effectOut));
    bindings[i].speed = clampSpeed(prefs.getUChar(keyS, 0));
  }

  prefs.end();
}

void save() {
  if (!prefs.begin("rfremote", false)) return;

  for (uint8_t i = 0; i < KNOWN_COUNT; i++) {
    char keyA[8], keyP[8], keyA2[8], keyP2[8], keyE[8], keyE2[8], keyS[8];
    snprintf(keyA, sizeof(keyA), "a%u", bindings[i].buttonId);
    snprintf(keyP, sizeof(keyP), "p%u", bindings[i].buttonId);
    snprintf(keyA2, sizeof(keyA2), "A%u", bindings[i].buttonId);
    snprintf(keyP2, sizeof(keyP2), "P%u", bindings[i].buttonId);
    snprintf(keyE, sizeof(keyE), "e%u", bindings[i].buttonId);
    snprintf(keyE2, sizeof(keyE2), "E%u", bindings[i].buttonId);
    snprintf(keyS, sizeof(keyS), "s%u", bindings[i].buttonId);
    prefs.putUChar(keyA, bindings[i].action);
    prefs.putUChar(keyP, bindings[i].param);
    prefs.putUChar(keyE, bindings[i].effectIn);
    prefs.putUChar(keyE2, bindings[i].effectOut);
    prefs.putUChar(keyS, bindings[i].speed);
    prefs.remove(keyA2);
    prefs.remove(keyP2);
  }

  prefs.end();
}

void handleButton(uint8_t buttonId) {
  if (SetupMenu::active()) {
    SetupMenu::handleButton(buttonId);
    return;
  }

  int idx = findIndex(buttonId);
  if (idx < 0) return;

  // ON ON ON OFF OFF OFF (≤1.5s between taps) → setup menu.
  // First ON unblanks right away; later ONs/OFFs only count toward the combo.
  if (buttonId == WIZ_ON) {
    if (comboOffCount > 0) {
      // ON after OFF phase started — end prior attempt, start fresh.
      clearComboArm();
    }
    if (!comboActive) {
      comboActive = true;
      comboOnCount = 0;
      comboOffCount = 0;
      comboOnFired = false;
    }
    if (comboOnCount >= COMBO_NEED) {
      // Extra ON after 3 — restart the ON run (keep display unblanked).
      comboOnCount = 0;
      comboOffCount = 0;
    }
    comboOnCount++;
    comboLastMs = millis();
    if (!comboOnFired) {
      comboOnFired = true;
      fireBindingIndex(idx);  // immediate unblank
    }
    return;
  }

  if (buttonId == WIZ_OFF && comboActive && comboOnCount == COMBO_NEED) {
    comboOffCount++;
    comboLastMs = millis();
    if (comboOffCount >= COMBO_NEED) {
      clearComboArm();
      SetupMenu::enter();
    }
    return;
  }

  if (comboActive) {
    clearComboArm();  // already unblanked on first ON
  }

  fireBindingIndex(idx);
}

uint8_t bindingCount() { return KNOWN_COUNT; }

Binding getBinding(uint8_t index) {
  if (index >= KNOWN_COUNT) {
    return Binding{0, ACTION_NONE, 0, ShowMode::LEFT, ShowMode::LEFT, 0};
  }
  return bindings[index];
}

bool setBinding(uint8_t buttonId, uint8_t action, uint8_t param,
                uint8_t effectIn, uint8_t effectOut, uint8_t speed) {
  int idx = findIndex(buttonId);
  if (idx < 0) return false;
  if (action > ACTION_IDLE_START) return false;
  if (action == ACTION_SELECT && param >= MAX_COUNTERS) return false;
  bindings[idx].action = action;
  bindings[idx].param = param;
  bindings[idx].effectIn = clampEffect(effectIn);
  bindings[idx].effectOut = clampEffect(effectOut);
  bindings[idx].speed = clampSpeed(speed);
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
    case ACTION_IDLE_START: return "idle_start";
    default: return "?";
  }
}

}  // namespace RemoteMap
