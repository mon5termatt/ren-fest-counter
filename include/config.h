#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Single scoreboard: one CS daisy-chain (name panels + number panels).
// Remote 1–4 / web switches which logical counter is shown on that display.
// ---------------------------------------------------------------------------
static constexpr bool SINGLE_DISPLAY = true;

// Full build: NAME=8, NUMBER=4 on one daisy-chain.
static constexpr uint8_t MODULES_NAME = 8;
static constexpr uint8_t MODULES_NUMBER = 4;
static constexpr uint8_t MODULES_PER_BOARD = MODULES_NAME + MODULES_NUMBER;

static constexpr uint8_t MAX_COUNTERS = 8;
static constexpr uint8_t NAME_MAX_LEN = 15;
static constexpr int32_t COUNT_MAX = 9999;

// ---------------------------------------------------------------------------
// SPI / MAX7219 — one CS for the whole name+number chain
// ---------------------------------------------------------------------------
static constexpr uint8_t PIN_DIN = 19;
static constexpr uint8_t PIN_CLK = 18;
static constexpr int8_t PIN_CS = 5;

// Kept as arrays for compatibility; only [0] is used in SINGLE_DISPLAY mode.
static constexpr int8_t CS_PINS[MAX_COUNTERS] = {
  PIN_CS, -1, -1, -1, -1, -1, -1, -1
};

// ---------------------------------------------------------------------------
// One DEC / INC pair on the board → adjusts the *active* (selected) counter
// ---------------------------------------------------------------------------
static constexpr int8_t BTN_DEC_PIN = 26;
static constexpr int8_t BTN_INC_PIN = 14;

// Legacy arrays (unused in single-display button path; left for reference)
static constexpr int8_t BTN_DEC_PINS[MAX_COUNTERS] = {
  BTN_DEC_PIN, -1, -1, -1, -1, -1, -1, -1
};
static constexpr int8_t BTN_INC_PINS[MAX_COUNTERS] = {
  BTN_INC_PIN, -1, -1, -1, -1, -1, -1, -1
};

static constexpr uint16_t BTN_DEBOUNCE_MS = 40;
static constexpr uint16_t BTN_HOLD_MS = 400;
static constexpr uint16_t BTN_REPEAT_MS = 100;

// ---------------------------------------------------------------------------
// SoftAP + OTA
// ---------------------------------------------------------------------------
static constexpr char AP_SSID[] = "RenFest-Counter";
static constexpr char AP_PASS[] = "renfest123";
static constexpr uint8_t AP_CHANNEL = 1;
static constexpr char OTA_HOSTNAME[] = "ren-fest-counter";
static constexpr char OTA_PASSWORD[] = "renfest123";

static constexpr uint8_t DEFAULT_INTENSITY = 4;
static constexpr uint8_t DEFAULT_ENABLED_COUNT = 4;  // slots 1–4 selectable out of the box
static constexpr uint8_t SCROLLER_MAX_LEN = 64;      // manual marquee message

// ---------------------------------------------------------------------------
// Idle auto-cycle (single display)
// ---------------------------------------------------------------------------
static constexpr uint32_t IDLE_TIMEOUT_MS = 30000;  // start cycling after 30s idle
static constexpr uint32_t IDLE_CYCLE_MS = 5000;     // default; override via web UI idleCycleSeconds
static constexpr uint16_t DOUBLE_TAP_MS = 400;      // WiZ remote double-tap window
