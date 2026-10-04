#include "wifi_mgr.h"
#include "config.h"

#include <WiFi.h>
#include <Preferences.h>
#include <cstring>

namespace {
  Preferences prefs;
  char savedSsid[33] = "";
  char savedPass[65] = "";
  bool haveCreds = false;
  uint32_t lastReconnectMs = 0;
  bool scanHeld = false;

  void loadCreds() {
    haveCreds = false;
    savedSsid[0] = '\0';
    savedPass[0] = '\0';
    if (prefs.begin("rfwifi", true)) {
      String s = prefs.getString("ssid", "");
      String p = prefs.getString("pass", "");
      prefs.end();
      if (s.length() > 0 && s.length() < sizeof(savedSsid)) {
        strncpy(savedSsid, s.c_str(), sizeof(savedSsid) - 1);
        strncpy(savedPass, p.c_str(), sizeof(savedPass) - 1);
        haveCreds = true;
        return;
      }
    }
    // Fall back to compile-time wifi_config.h
    if (STA_SSID[0] != '\0') {
      strncpy(savedSsid, STA_SSID, sizeof(savedSsid) - 1);
      savedSsid[sizeof(savedSsid) - 1] = '\0';
      strncpy(savedPass, STA_PASS, sizeof(savedPass) - 1);
      savedPass[sizeof(savedPass) - 1] = '\0';
      haveCreds = true;
    }
  }

  void saveCreds(const char* ssid, const char* pass) {
    if (!prefs.begin("rfwifi", false)) return;
    prefs.putString("ssid", ssid ? ssid : "");
    prefs.putString("pass", pass ? pass : "");
    prefs.end();
    strncpy(savedSsid, ssid ? ssid : "", sizeof(savedSsid) - 1);
    savedSsid[sizeof(savedSsid) - 1] = '\0';
    strncpy(savedPass, pass ? pass : "", sizeof(savedPass) - 1);
    savedPass[sizeof(savedPass) - 1] = '\0';
    haveCreds = savedSsid[0] != '\0';
  }

  void eraseCreds() {
    if (prefs.begin("rfwifi", false)) {
      prefs.clear();
      prefs.end();
    }
    savedSsid[0] = '\0';
    savedPass[0] = '\0';
    haveCreds = false;
  }
}

namespace WifiMgr {

void begin() {
  loadCreds();

  // SoftAP stays up for config / SoftAP OTA / ESP-NOW channel anchor.
  // When STA joins a router, SoftAP moves to that channel (ESP-NOW follows).
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL);
  Serial.print(F("[wifi] AP "));
  Serial.print(AP_SSID);
  Serial.print(F("  IP "));
  Serial.println(WiFi.softAPIP());

  if (haveCreds) {
    Serial.print(F("[wifi] joining "));
    Serial.println(savedSsid);
    WiFi.begin(savedSsid, savedPass);
  }
}

void loop() {
  if (!haveCreds) return;
  if (WiFi.status() == WL_CONNECTED) return;

  const uint32_t now = millis();
  if (now - lastReconnectMs < 15000) return;
  lastReconnectMs = now;
  Serial.print(F("[wifi] reconnect "));
  Serial.println(savedSsid);
  WiFi.disconnect(false);
  WiFi.begin(savedSsid, savedPass);
}

bool connectSta(const char* ssid, const char* password) {
  if (!ssid || ssid[0] == '\0' || strlen(ssid) > 32) return false;
  saveCreds(ssid, password ? password : "");
  lastReconnectMs = millis();
  WiFi.disconnect(false);
  delay(50);
  WiFi.begin(savedSsid, savedPass);
  return true;
}

void clearSta() {
  eraseCreds();
  WiFi.disconnect(true);
}

void disconnectSta() {
  WiFi.disconnect(false);
}

bool staConfigured() { return haveCreds; }

bool staConnected() { return WiFi.status() == WL_CONNECTED; }

const char* staSsid() {
  if (WiFi.status() == WL_CONNECTED) {
    static char cur[33];
    String s = WiFi.SSID();
    strncpy(cur, s.c_str(), sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = '\0';
    return cur;
  }
  return haveCreds ? savedSsid : "";
}

String staIp() {
  if (WiFi.status() != WL_CONNECTED) return String();
  return WiFi.localIP().toString();
}

String apIp() { return WiFi.softAPIP().toString(); }

int32_t staRssi() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  return WiFi.RSSI();
}

const char* staStatusText() {
  switch (WiFi.status()) {
    case WL_CONNECTED: return "connected";
    case WL_NO_SSID_AVAIL: return "ssid not found";
    case WL_CONNECT_FAILED: return "connect failed";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED: return haveCreds ? "disconnected" : "idle";
    case WL_IDLE_STATUS: return "idle";
    default: return "connecting";
  }
}

int16_t scanNetworks() {
  if (scanHeld) {
    WiFi.scanDelete();
    scanHeld = false;
  }
  // Async false — UI waits on this request
  const int16_t n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/false);
  scanHeld = (n > 0);
  return n;
}

int16_t scanCount() {
  const int16_t n = WiFi.scanComplete();
  if (n < 0) return 0;
  return n;
}

const char* scanSsid(uint8_t i) {
  static char buf[33];
  String s = WiFi.SSID(i);
  strncpy(buf, s.c_str(), sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  return buf;
}

int32_t scanRssi(uint8_t i) { return WiFi.RSSI(i); }

bool scanSecure(uint8_t i) {
  return WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
}

}  // namespace WifiMgr
