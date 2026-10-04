#pragma once

// Wi-Fi / SoftAP / OTA — edit STA_* to auto-join a router on boot.
// SoftAP stays up either way for the web UI and ESP-NOW channel.

// Station (home / venue router). Leave SSID empty to skip STA join
// until configured in the web UI (then stored in NVS).
static constexpr char STA_SSID[] = "FamilyLAN";
static constexpr char STA_PASS[] = "Ilovetech";

// SoftAP (config / OTA / ESP-NOW anchor)
static constexpr char AP_SSID[] = "RenFest-Counter";
static constexpr char AP_PASS[] = "renfest123";
static constexpr uint8_t AP_CHANNEL = 1;

static constexpr char OTA_HOSTNAME[] = "ren-fest-counter";
static constexpr char OTA_PASSWORD[] = "renfest123";
