#pragma once

#include <Arduino.h>

// BMP Badge LED ShowModes (english.xml cb_showmode0) + Random picker
namespace ShowMode {
  enum : uint8_t {
    LEFT = 0,
    RIGHT = 1,
    UP = 2,
    DOWN = 3,
    FREEZE = 4,
    ANIMATION = 5,
    PILING = 6,
    SPLITE = 7,
    LASER_LEFT = 8,   // legacy id 8 was "LASER"
    SMOTH = 9,
    ROTATE = 10,
    RANDOM = 11,
    LASER_RIGHT = 12,
    LASER_UP = 13,
    LASER_DOWN = 14,
    COUNT = 15
  };

  // Concrete modes Random may pick (excludes FREEZE/None and RANDOM)
  constexpr uint8_t RANDOM_POOL[] = {
    LEFT, RIGHT, UP, DOWN, ANIMATION, PILING, SPLITE,
    LASER_LEFT, LASER_RIGHT, LASER_UP, LASER_DOWN, SMOTH, ROTATE
  };
  constexpr uint8_t RANDOM_POOL_COUNT = sizeof(RANDOM_POOL) / sizeof(RANDOM_POOL[0]);

  inline bool valid(uint8_t m) { return m < COUNT; }

  inline bool isLaser(uint8_t m) {
    return m == LASER_LEFT || m == LASER_RIGHT || m == LASER_UP || m == LASER_DOWN;
  }

  // Resolve RANDOM → a fixed ShowMode for this play (never None/Freeze)
  inline uint8_t resolve(uint8_t m) {
    if (!valid(m)) return LEFT;
    if (m == RANDOM) {
      return RANDOM_POOL[random(RANDOM_POOL_COUNT)];
    }
    return m;
  }

  inline const char* label(uint8_t m) {
    switch (m) {
      case LEFT: return "Left";
      case RIGHT: return "Right";
      case UP: return "Up";
      case DOWN: return "Down";
      case FREEZE: return "None";
      case ANIMATION: return "Animation";
      case PILING: return "Piling";
      case SPLITE: return "Splite";
      case LASER_LEFT: return "Laser Left";
      case SMOTH: return "Smoth";
      case ROTATE: return "Rotate";
      case RANDOM: return "Random";
      case LASER_RIGHT: return "Laser Right";
      case LASER_UP: return "Laser Up";
      case LASER_DOWN: return "Laser Down";
      default: return "?";
    }
  }
}
