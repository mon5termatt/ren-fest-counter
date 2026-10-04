#include "setup_menu.h"
#include "config.h"
#include "counters.h"
#include "display.h"
#include "idle_cycle.h"
#include "wifi_config.h"
#include "wifi_mgr.h"
#include "wiz_remote.h"
#include <cstdio>
#include <cstring>

namespace {
  constexpr uint8_t WIZ_ON = 1;
  constexpr uint8_t WIZ_OFF = 2;
  constexpr uint8_t WIZ_NIGHT = 3;
  constexpr uint8_t WIZ_BRIGHT_DOWN = 8;
  constexpr uint8_t WIZ_BRIGHT_UP = 9;

  enum Page : uint8_t {
    PageAp = 0,
    PageSta = 1,
    PageRemote = 2,
    PageBright = 3,
    PageBlank = 4,
    PageBlankPct = 5,
    PageTestCheck = 6,
    PageTestAll = 7,
    PageCount = 8,
  };

  bool inMenu = false;
  uint8_t page = PageAp;

  bool isTestPage() {
    return page == PageTestCheck || page == PageTestAll;
  }

  void paintCurrent() {
    if (page == PageTestCheck) {
      Display::showTestCheckerboard();
      return;
    }
    if (page == PageTestAll) {
      Display::showTestAllOn();
      return;
    }

    char top[SCROLLER_MAX_LEN + 1];
    char bot[SCROLLER_BOTTOM_MAX_LEN + 1];
    top[0] = '\0';
    bot[0] = '\0';

    switch (page) {
      case PageAp: {
        String ip = WifiMgr::apIp();
        snprintf(top, sizeof(top), "%s", ip.c_str());
        strncpy(bot, AP_SSID, sizeof(bot) - 1);
        bot[sizeof(bot) - 1] = '\0';
        break;
      }
      case PageSta:
        if (WifiMgr::staConnected()) {
          String ip = WifiMgr::staIp();
          snprintf(top, sizeof(top), "%s", ip.c_str());
          const char* ssid = WifiMgr::staSsid();
          strncpy(bot, (ssid && ssid[0]) ? ssid : "STA", sizeof(bot) - 1);
        } else {
          strncpy(top, "-", sizeof(top) - 1);
          strncpy(bot, "none", sizeof(bot) - 1);
        }
        bot[sizeof(bot) - 1] = '\0';
        break;
      case PageRemote: {
        const char* linked = Counters::linkedRemoteMac();
        const char* seen = WizRemote::lastSeenMac();
        const char* id = (linked && linked[0]) ? linked
                         : (seen && seen[0])   ? seen
                                               : "-";
        snprintf(top, sizeof(top), "%s", id);
        strncpy(bot, "remote", sizeof(bot) - 1);
        bot[sizeof(bot) - 1] = '\0';
        break;
      }
      case PageBright:
        strncpy(top, "Bright", sizeof(top) - 1);
        snprintf(bot, sizeof(bot), "%u", static_cast<unsigned>(Counters::intensity()));
        break;
      case PageBlank:
        strncpy(top, "Blank", sizeof(top) - 1);
        strncpy(bot, Counters::blanked() ? "ON" : "OFF", sizeof(bot) - 1);
        break;
      case PageBlankPct: {
        strncpy(top, "Blank%", sizeof(top) - 1);
        const uint8_t v = Counters::blankBrightnessPercent();
        if (v == 0) strncpy(bot, "OFF", sizeof(bot) - 1);
        else snprintf(bot, sizeof(bot), "%u", static_cast<unsigned>(v));
        break;
      }
      default:
        break;
    }
    top[sizeof(top) - 1] = '\0';
    bot[sizeof(bot) - 1] = '\0';
    Display::showStatusLines(top, bot);
  }

  void bumpBlankPct(int delta) {
    int v = static_cast<int>(Counters::blankBrightnessPercent()) + delta;
    if (v < 0) v = 0;
    if (v > 15) v = 15;
    Counters::setBlankBrightnessPercent(static_cast<uint8_t>(v));
    Display::applyBlanked();
    // showStatusLines wakes panels even if blank level is OFF (full shutdown).
    paintCurrent();
  }

  void bumpBright(int delta) {
    int v = static_cast<int>(Counters::intensity()) + delta;
    if (v < 0) v = 0;
    if (v > 15) v = 15;
    Counters::setIntensity(static_cast<uint8_t>(v));
    Display::applyIntensity();
    paintCurrent();
  }

  void nextPage(int delta) {
    int p = static_cast<int>(page) + delta;
    while (p < 0) p += PageCount;
    page = static_cast<uint8_t>(p % PageCount);
    paintCurrent();
  }
}

namespace SetupMenu {

void begin() {
  inMenu = false;
  page = PageAp;
}

bool active() { return inMenu; }

void enter() {
  inMenu = true;
  page = PageAp;
  IdleCycle::noteActivity();
  if (Display::scrollerActive()) {
    Display::stopManualScroller(false);
  }
  Serial.println(F("[setup] enter"));
  paintCurrent();
}

void exit() {
  if (!inMenu) return;
  inMenu = false;
  IdleCycle::noteActivity();
  Display::refreshAll();
  Counters::saveIfDirty();
  Serial.println(F("[setup] exit"));
}

bool handleButton(uint8_t buttonId) {
  if (!inMenu) return false;

  switch (buttonId) {
    case WIZ_NIGHT:
    case WIZ_OFF:
      exit();
      return true;

    case WIZ_ON:
      // Stay in menu; refresh current page.
      paintCurrent();
      return true;

    case WIZ_BRIGHT_UP:
      if (page == PageBright || isTestPage()) {
        bumpBright(1);
      } else if (page == PageBlank) {
        Counters::setBlanked(!Counters::blanked());
        Display::applyBlanked();
        paintCurrent();
      } else if (page == PageBlankPct) {
        bumpBlankPct(1);
      } else {
        nextPage(1);
      }
      return true;

    case WIZ_BRIGHT_DOWN:
      if (page == PageBright || isTestPage()) {
        bumpBright(-1);
      } else if (page == PageBlank) {
        Counters::setBlanked(!Counters::blanked());
        Display::applyBlanked();
        paintCurrent();
      } else if (page == PageBlankPct) {
        bumpBlankPct(-1);
      } else {
        nextPage(-1);
      }
      return true;

    default:
      // Digits / other: advance page.
      nextPage(1);
      return true;
  }
}

}  // namespace SetupMenu
