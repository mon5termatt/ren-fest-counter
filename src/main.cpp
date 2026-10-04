#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "counters.h"
#include "display.h"
#include "remote_map.h"
#include "wiz_remote.h"
#include "web_ui.h"
#include "ota.h"
#include "idle_cycle.h"
#include "wifi_mgr.h"

namespace {
  uint32_t lastSaveMs = 0;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("Ren Fest Counter"));

  Counters::begin();
  RemoteMap::begin();
  Display::begin();
  IdleCycle::begin();

  WifiMgr::begin();

  // ESP-NOW after SoftAP so it shares the AP channel
  WizRemote::begin();
  Ota::begin();
  WebUI::begin();

  Display::refreshAll();
}

void loop() {
  Ota::loop();
  if (Ota::busy()) return;  // don't starve OTA transfer

  WifiMgr::loop();
  WizRemote::loop();
  RemoteMap::loop();
  IdleCycle::loop();
  Display::loop();
  WebUI::loop();

  uint32_t now = millis();
  if (now - lastSaveMs > 2000) {
    lastSaveMs = now;
    Counters::saveIfDirty();
  }
}
