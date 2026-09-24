#include "ota.h"
#include "config.h"
#include <ArduinoOTA.h>
#include <WiFi.h>

namespace {
  bool updating = false;
}

namespace Ota {

bool busy() { return updating; }

void begin() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  // SoftAP OTA: device pulls firmware from the client on 192.168.4.x
  ArduinoOTA.setRebootOnSuccess(true);

  ArduinoOTA.onStart([]() {
    updating = true;
    Serial.println(F("[ota] start — pause other work"));
  });
  ArduinoOTA.onEnd([]() {
    Serial.println(F("\n[ota] end"));
    updating = false;
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static unsigned int lastPct = 255;
    unsigned int pct = (progress * 100) / (total ? total : 1);
    if (pct != lastPct) {
      lastPct = pct;
      Serial.printf("[ota] %u%%\r", pct);
    }
  });
  ArduinoOTA.onError([](ota_error_t error) {
    updating = false;
    Serial.printf("\n[ota] error %u\n", error);
  });

  ArduinoOTA.begin();
  Serial.print(F("[ota] SoftAP "));
  Serial.print(WiFi.softAPIP());
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("  STA "));
    Serial.print(WiFi.localIP());
  }
  Serial.println(F(" — HTTP POST /update or: pio run -e ota -t upload"));
}

void loop() {
  ArduinoOTA.handle();
}

}  // namespace Ota
