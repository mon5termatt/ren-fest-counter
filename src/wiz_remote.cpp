#include "wiz_remote.h"
#include "config.h"
#include "counters.h"
#include "remote_map.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <cstring>
#include <cstdio>

namespace {
  typedef struct __attribute__((packed)) {
    uint8_t program;
    uint8_t seq[4];
    uint8_t dt1;
    uint8_t button;
    uint8_t dt2;
    uint8_t batLevel;
    uint8_t byte10;
    uint8_t byte11;
    uint8_t byte12;
    uint8_t byte13;
  } WizMoteMessage;

  static_assert(sizeof(WizMoteMessage) == 13, "WiZ mote packet must be 13 bytes");

  volatile int16_t queuedButton = -1;
  uint32_t lastSeq = UINT32_MAX;
  char lastMac[13] = "";

  void macToHex(const uint8_t* mac, char* out12) {
    snprintf(out12, 13, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }

  bool macAllowed(const uint8_t* mac) {
    const char* linked = Counters::linkedRemoteMac();
    if (!linked || linked[0] == '\0') return true;
    char hex[13];
    macToHex(mac, hex);
    return strcasecmp(hex, linked) == 0;
  }

  void handlePacket(const uint8_t* mac, const uint8_t* data, int len) {
    if (!mac || !data || len != (int)sizeof(WizMoteMessage)) return;

    // Always record last-seen for the link UI
    macToHex(mac, lastMac);

    if (!macAllowed(mac)) return;

    const WizMoteMessage* msg = reinterpret_cast<const WizMoteMessage*>(data);
    uint32_t seq = (uint32_t)msg->seq[0] |
                   ((uint32_t)msg->seq[1] << 8) |
                   ((uint32_t)msg->seq[2] << 16) |
                   ((uint32_t)msg->seq[3] << 24);
    if (seq == lastSeq) return;
    lastSeq = seq;
    queuedButton = msg->button;
  }

#if defined(ESP_IDF_VERSION_MAJOR) && (ESP_IDF_VERSION_MAJOR >= 5)
  void onDataRecvV2(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (!info) return;
    handlePacket(info->src_addr, data, len);
  }
#else
  void onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    handlePacket(mac, data, len);
  }
#endif
}

namespace WizRemote {

void begin() {
  if (esp_now_init() != ESP_OK) {
    Serial.println(F("[wiz] esp_now_init failed"));
    return;
  }

#if defined(ESP_IDF_VERSION_MAJOR) && (ESP_IDF_VERSION_MAJOR >= 5)
  esp_now_register_recv_cb(onDataRecvV2);
#else
  esp_now_register_recv_cb(onDataRecv);
#endif

  Serial.println(F("[wiz] ESP-NOW listening for WiZ mote"));
}

void loop() {
  int16_t btn = queuedButton;
  if (btn < 0) return;
  queuedButton = -1;
  RemoteMap::handleButton(static_cast<uint8_t>(btn));
}

bool takeButton(uint8_t& buttonOut) {
  int16_t btn = queuedButton;
  if (btn < 0) return false;
  queuedButton = -1;
  buttonOut = static_cast<uint8_t>(btn);
  return true;
}

const char* lastSeenMac() { return lastMac; }

bool hasLastSeenMac() { return lastMac[0] != '\0'; }

}  // namespace WizRemote
