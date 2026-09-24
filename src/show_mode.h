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
    LASER = 8,
    SMOTH = 9,
    ROTATE = 10,
    RANDOM = 11,   // pick a concrete mode each play
    COUNT = 12
  };

  // Concrete badge modes (excludes RANDOM)
  constexpr uint8_t FIXED_COUNT = RANDOM;

  inline bool valid(uint8_t m) { return m < COUNT; }

  // Resolve RANDOM → a fixed ShowMode for this play
  inline uint8_t resolve(uint8_t m) {
    if (!valid(m)) return LEFT;
    if (m == RANDOM) {
      return static_cast<uint8_t>(random(FIXED_COUNT));
    }
    return m;
  }

  inline const char* label(uint8_t m) {
    switch (m) {
      case LEFT: return "Left";
      case RIGHT: return "Right";
      case UP: return "Up";
      case DOWN: return "Down";
      case FREEZE: return "Freeze";
      case ANIMATION: return "Animation";
      case PILING: return "Piling";
      case SPLITE: return "Splite";
      case LASER: return "Laser";
      case SMOTH: return "Smoth";
      case ROTATE: return "Rotate";
      case RANDOM: return "Random";
      default: return "?";
    }
  }
}
