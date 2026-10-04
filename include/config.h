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

// 8×2 frame: top (name) row is rotated 180° relative to the number row.
static constexpr bool NAME_ZONE_FLIP_UD = true;
static constexpr bool NAME_ZONE_FLIP_LR = true;

static constexpr uint8_t MAX_COUNTERS = 8;
static constexpr uint8_t NAME_MAX_LEN = 15;
static constexpr int32_t COUNT_MAX = 9999;

// ---------------------------------------------------------------------------
// SPI / MAX7219 — one CS for the whole name+number chain
// ---------------------------------------------------------------------------
#if defined(ARDUINO_XIAO_ESP32C3)
// XIAO ESP32C3 — SPI on D7/D8/D10 edge (D9 left free / BOOT); VCC/GND on their own pads
static constexpr uint8_t PIN_DIN = 10;     // D10
static constexpr uint8_t PIN_CLK = 8;      // D8
static constexpr int8_t PIN_CS = 20;       // D7
#else
// ESP32-WROOM-32 defaults
static constexpr uint8_t PIN_DIN = 19;
static constexpr uint8_t PIN_CLK = 18;
static constexpr int8_t PIN_CS = 5;
#endif

// Kept as arrays for compatibility; only [0] is used in SINGLE_DISPLAY mode.
static constexpr int8_t CS_PINS[MAX_COUNTERS] = {
  PIN_CS, -1, -1, -1, -1, -1, -1, -1
};

#include "wifi_config.h"

static constexpr uint8_t DEFAULT_INTENSITY = 4;
static constexpr uint8_t DEFAULT_ENABLED_COUNT = 4;  // slots 1–4 selectable out of the box
static constexpr uint8_t SCROLLER_MAX_LEN = 64;      // top marquee message
static constexpr uint8_t SCROLLER_BOTTOM_MAX_LEN = 16; // bottom line (number zone)
static constexpr uint8_t MAX_SCROLL_MESSAGES = 8;
static constexpr uint8_t MAX_IDLE_PLAYLIST = 16;

// ---------------------------------------------------------------------------
// Idle auto-cycle (single display)
// ---------------------------------------------------------------------------
static constexpr uint32_t IDLE_TIMEOUT_MS = 30000;  // start cycling after 30s idle
static constexpr uint32_t IDLE_CYCLE_MS = 5000;     // default; override via web UI idleCycleSeconds
static constexpr uint16_t DOUBLE_TAP_MS = 400;      // WiZ remote double-tap window
