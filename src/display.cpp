#include "display.h"
#include "counters.h"
#include "show_mode.h"
#include "wiz_remote.h"
#include "local_buttons.h"
#include "remote_map.h"
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <cstring>

namespace {
  constexpr MD_MAX72XX::moduleType_t HW = MD_MAX72XX::FC16_HW;
  constexpr uint8_t ZONE_COUNT =
      (MODULES_NAME > 0 && MODULES_NUMBER > 0) ? 2 : 1;
  constexpr uint8_t COLS_PER_MOD = 8;

  MD_Parola* board = nullptr;
  bool inited = false;

  char nameBuf[NAME_MAX_LEN + 1];
  char numBuf[8];

  void formatCount(int32_t count) {
    snprintf(numBuf, sizeof(numBuf), "#%ld", (long)count);
  }

  uint32_t flashUntil = 0;
  bool flashOn = false;

  // Separate gens so number updates don't cancel an in-flight name transition.
  enum class AnimSlot : uint8_t { Name, Num };
  uint32_t nameAnimGen = 0;
  uint32_t numAnimGen = 0;

  uint32_t beginAnim(AnimSlot slot) {
    return (slot == AnimSlot::Name) ? ++nameAnimGen : ++numAnimGen;
  }

  bool animCurrent(AnimSlot slot, uint32_t gen) {
    return gen == ((slot == AnimSlot::Name) ? nameAnimGen : numAnimGen);
  }

  void pollDuringAnim() {
    WizRemote::loop();
    LocalButtons::loop();
    RemoteMap::loop();
    yield();
  }

  int8_t numberZone() {
    if (MODULES_NAME > 0 && MODULES_NUMBER > 0) return 1;
    if (MODULES_NAME == 0 && MODULES_NUMBER > 0) return 0;
    return -1;
  }

  int8_t nameZoneId() {
    if (MODULES_NAME > 0 && MODULES_NUMBER > 0) return 0;
    if (MODULES_NAME > 0 && MODULES_NUMBER == 0) return 0;
    return -1;
  }

  // Zone used for count/boot/fireworks when only one strip is wired
  int8_t primaryAnimZone() {
    const int8_t cz = numberZone();
    if (cz >= 0) return cz;
    return nameZoneId();
  }

  uint8_t percentToIntensity(uint8_t pct) {
    if (pct == 0) return 0;
    if (pct >= 100) return 15;
    return static_cast<uint8_t>((pct * 15 + 50) / 100);
  }

  uint8_t effectiveIntensity() {
    if (!Counters::blanked()) return Counters::intensity();
    return percentToIntensity(Counters::blankBrightnessPercent());
  }

  textEffect_t toParolaEffect(uint8_t mode) {
    switch (mode) {
      case ShowMode::LEFT: return PA_SCROLL_LEFT;
      case ShowMode::RIGHT: return PA_SCROLL_RIGHT;
      case ShowMode::UP: return PA_SCROLL_UP;
      case ShowMode::DOWN: return PA_SCROLL_DOWN;
      case ShowMode::FREEZE: return PA_PRINT;
      case ShowMode::ANIMATION: return PA_RANDOM;
      case ShowMode::PILING: return PA_GROW_UP;
      case ShowMode::SPLITE: return PA_OPENING;
      case ShowMode::LASER: return PA_WIPE_CURSOR;
      case ShowMode::SMOTH: return PA_FADE;
      case ShowMode::ROTATE: return PA_MESH;
      default: return PA_SCROLL_LEFT;
    }
  }

  uint8_t clampEffect(uint8_t mode) {
    return ShowMode::valid(mode) ? mode : ShowMode::LEFT;
  }

  uint8_t resolveEffect(uint8_t mode) {
    return ShowMode::resolve(clampEffect(mode));
  }

  bool isMilestone(int32_t count) {
    if (count <= 0) return false;
    if (count == 10 || count == 25) return true;
    return (count % 50) == 0;
  }

  bool displayVisible() {
    if (!Counters::blanked()) return true;
    return Counters::blankBrightnessPercent() > 0;
  }

  constexpr uint16_t TEETER_END_PAUSE_MS = 600;
  // Max columns for a name glyph stream (15 chars * ~6px + spacing)
  constexpr uint16_t NAME_COL_BUF = 128;

  // teeterSpeed 1..10 → frame ms ~80..15
  uint16_t teeterFrameMs() {
    uint8_t s = Counters::teeterSpeed();
    if (s < 1) s = 1;
    if (s > 10) s = 10;
    return static_cast<uint16_t>(90 - (s * 7));
  }

  // Transition scroll: frame delay (lower = faster)
  uint16_t transitionFrameMs() {
    uint8_t s = Counters::scrollSpeed();
    if (s < 1) s = 1;
    if (s > 10) s = 10;
    // ~18ms at 1 → ~3ms at 10
    return static_cast<uint16_t>(20 - s);
  }

  // Columns advanced per transition frame (snappy even on 64-col zones)
  uint8_t transitionStepCols() {
    uint8_t s = Counters::scrollSpeed();
    if (s < 1) s = 1;
    if (s > 10) s = 10;
    // 2 at speed 1 → 8 at speed 10
    return static_cast<uint8_t>(1 + s);
  }

  // Parola effect speed (lower = faster); scale with scrollSpeed (transitions)
  uint16_t parolaScrollSpeed(uint16_t base) {
    uint8_t s = Counters::scrollSpeed();
    if (s < 1) s = 1;
    if (s > 10) s = 10;
    const int scaled = static_cast<int>(base) * 5 / static_cast<int>(s);
    if (scaled < 4) return 4;
    if (scaled > 60) return 60;
    return static_cast<uint16_t>(scaled);
  }

  struct NameTeeter {
    bool active = false;
    uint8_t zone = 0;
    uint16_t zoneStart = 0;
    uint16_t zoneCols = 0;
    uint16_t textCols = 0;
    uint16_t overflow = 0;
    int16_t offset = 0;
    int8_t dir = 1;
    uint32_t nextMs = 0;
    bool pausing = false;
    uint8_t colBuf[NAME_COL_BUF];
  } teeter;

  // Last static name blit (when not teetering) — restored after Parola number anims.
  bool nameStaticValid = false;
  int16_t nameStaticOrigin = 0;

  // Manual marquee (name zone loops; number cleared)
  bool scrollerOn = false;
  uint8_t scrollerZone = 0;
  char scrollerBuf[SCROLLER_MAX_LEN + 1] = "";
  textEffect_t scrollerDir = PA_SCROLL_LEFT;

  void setNameModulesIntensity(uint8_t intensity) {
    MD_MAX72XX* mx = board->getGraphicObject();
    if (!mx || MODULES_NAME == 0) return;
    mx->control(0, MODULES_NAME - 1, MD_MAX72XX::INTENSITY, intensity);
  }

  uint16_t zoneWidthCols(uint8_t z) {
    uint16_t startCol = 0, endCol = 0;
    board->getDisplayExtent(z, startCol, endCol);
    if (endCol < startCol) return 0;
    return static_cast<uint16_t>(endCol - startCol + 1);
  }

  void stopNameTeeter() { teeter.active = false; }

  void restoreNameBlit();  // after blit helpers

  // Declared later; used by non-scroll name transitions.
  bool runEffectAnim(uint8_t zone, const char* text, uint8_t effect,
                     uint16_t speed, uint16_t pauseMs, uint32_t gen, AnimSlot slot);

  // Build column buffer: 1px between letters; no extra gaps around spaces
  // (space glyph alone is the word gap). "CAPTAIN JACK" → 64 cols on 8 modules.
  uint16_t buildNameColumns(const char* text, uint8_t* out, uint16_t outMax) {
    MD_MAX72XX* mx = board->getGraphicObject();
    if (!mx || !text || !out || outMax == 0) return 0;

    uint16_t n = 0;
    const size_t len = strlen(text);
    for (size_t i = 0; i < len; i++) {
      uint8_t glyph[10];
      uint8_t w = mx->getChar(static_cast<uint8_t>(text[i]), sizeof(glyph), glyph);
      if (w > sizeof(glyph)) w = sizeof(glyph);
      for (uint8_t c = 0; c < w && n < outMax; c++) {
        out[n++] = glyph[c];
      }
      // Letter spacing only between two non-space glyphs
      if (i + 1 < len && text[i] != ' ' && text[i + 1] != ' ' && n < outMax) {
        out[n++] = 0;
      }
    }
    return n;
  }

  // origin = source column of the leftmost visible pixel (negative pads left).
  void blitNameWindow(uint8_t /*zone*/, uint16_t zoneStart, uint16_t zoneCols,
                      const uint8_t* colBuf, uint16_t textCols, int16_t origin) {
    MD_MAX72XX* mx = board->getGraphicObject();
    if (!mx || zoneCols == 0) return;

    const uint16_t zoneEnd = zoneStart + zoneCols - 1;
    for (uint16_t i = 0; i < zoneCols; i++) {
      const int32_t src = static_cast<int32_t>(origin) + static_cast<int32_t>(i);
      const uint8_t v =
          (src >= 0 && src < static_cast<int32_t>(textCols)) ? colBuf[src] : 0;
      mx->setColumn(zoneEnd - i, v);
    }
    mx->update();
  }

  void blitTeeterFrame() {
    if (!teeter.active) return;
    blitNameWindow(teeter.zone, teeter.zoneStart, teeter.zoneCols, teeter.colBuf,
                   teeter.textCols, teeter.offset);
  }

  void restoreNameBlit() {
    if (teeter.active) {
      blitTeeterFrame();
      return;
    }
    if (!nameStaticValid || teeter.textCols == 0 || teeter.zoneCols == 0) return;
    blitNameWindow(teeter.zone, teeter.zoneStart, teeter.zoneCols, teeter.colBuf,
                   teeter.textCols, nameStaticOrigin);
  }

  void paintNameStatic(uint8_t z, const uint8_t* colBuf, uint16_t textCols) {
    uint16_t startCol = 0, endCol = 0;
    board->getDisplayExtent(z, startCol, endCol);
    const uint16_t zCols = zoneWidthCols(z);
    const int16_t origin =
        (textCols >= zCols) ? 0
                            : static_cast<int16_t>(-((static_cast<int16_t>(zCols) -
                                                      static_cast<int16_t>(textCols)) /
                                                     2));
    teeter.zone = z;
    teeter.zoneStart = startCol;
    teeter.zoneCols = zCols;
    teeter.textCols = textCols;
    if (colBuf != teeter.colBuf && textCols > 0) {
      memcpy(teeter.colBuf, colBuf, textCols);
    }
    nameStaticValid = true;
    nameStaticOrigin = origin;
    // Blit fills every zone column — no clear (avoids a blank flash).
    blitNameWindow(z, startCol, zCols, teeter.colBuf, textCols, origin);
  }

  void startNameTeeter(uint8_t z, const char* text) {
    uint16_t startCol = 0, endCol = 0;
    board->getDisplayExtent(z, startCol, endCol);
    const uint16_t zCols = zoneWidthCols(z);
    const uint16_t tCols = buildNameColumns(text, teeter.colBuf, NAME_COL_BUF);

    nameStaticValid = false;
    teeter.active = true;
    teeter.zone = z;
    teeter.zoneStart = startCol;
    teeter.zoneCols = zCols;
    teeter.textCols = tCols;
    teeter.overflow = (tCols > zCols) ? static_cast<uint16_t>(tCols - zCols) : 0;
    teeter.offset = 0;
    teeter.dir = 1;
    teeter.pausing = true;
    teeter.nextMs = millis() + TEETER_END_PAUSE_MS;
    blitTeeterFrame();
  }

  void paintName(uint8_t z, const char* text) {
    const uint16_t tCols = buildNameColumns(text, teeter.colBuf, NAME_COL_BUF);
    const uint16_t zCols = zoneWidthCols(z);
    if (tCols <= zCols) {
      stopNameTeeter();
      paintNameStatic(z, teeter.colBuf, tCols);
    } else {
      board->displayClear(z);
      startNameTeeter(z, text);
    }
  }

  // Advance Parola number zone; re-blit name so displayAnimate doesn't wipe it.
  bool tickNumberKeepName(int8_t cz, uint32_t numGen, int16_t nameOrigin) {
    if (cz < 0) return true;
    if (!animCurrent(AnimSlot::Num, numGen)) return false;
    if (!board->getZoneStatus(cz)) {
      board->displayAnimate();
      if (teeter.zoneCols > 0 && teeter.textCols > 0) {
        blitNameWindow(teeter.zone, teeter.zoneStart, teeter.zoneCols, teeter.colBuf,
                       teeter.textCols, nameOrigin);
      }
    }
    return animCurrent(AnimSlot::Num, numGen);
  }

  bool finishNumberKeepName(int8_t cz, uint32_t numGen, int16_t nameOrigin) {
    if (cz < 0) return true;
    while (!board->getZoneStatus(cz)) {
      if (!tickNumberKeepName(cz, numGen, nameOrigin)) return false;
      pollDuringAnim();
    }
    if (teeter.zoneCols > 0 && teeter.textCols > 0) {
      blitNameWindow(teeter.zone, teeter.zoneStart, teeter.zoneCols, teeter.colBuf,
                     teeter.textCols, nameOrigin);
    }
    return animCurrent(AnimSlot::Num, numGen);
  }

  bool startNumberInAnim(int8_t cz, const char* num, uint8_t resolved, uint32_t numGen) {
    if (cz < 0 || !num) return true;
    if (!animCurrent(AnimSlot::Num, numGen)) return false;
    const textEffect_t inEff = toParolaEffect(resolved);
    uint16_t spd = parolaScrollSpeed(30);
    if (resolved == ShowMode::ANIMATION) spd = 4;
    board->setCharSpacing(cz, 1);
    board->displayClear(cz);
    if (inEff == PA_PRINT || resolved == ShowMode::FREEZE) {
      board->displayZoneText(cz, num, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
    } else if (resolved == ShowMode::SMOTH) {
      board->displayZoneText(cz, num, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
    } else {
      board->displayZoneText(cz, num, PA_CENTER, spd, 0, inEff, PA_NO_EFFECT);
    }
    board->displayReset(cz);
    // Settle PRINT immediately so dual scroll can keep name on top.
    if (inEff == PA_PRINT || resolved == ShowMode::FREEZE || resolved == ShowMode::SMOTH) {
      while (!board->getZoneStatus(cz)) {
        if (!animCurrent(AnimSlot::Num, numGen)) return false;
        board->displayAnimate();
        pollDuringAnim();
      }
    }
    return animCurrent(AnimSlot::Num, numGen);
  }

  // Name transitions: LEFT/RIGHT use column scroll (correct word gaps).
  // Other effects use Parola (or a fade), then settle via paintName.
  // When numZone >= 0, number animates in parallel with the same effect.
  bool runNameInAnim(uint8_t z, const char* text, uint8_t effect, uint32_t gen,
                     int8_t numZone = -1, const char* numText = nullptr,
                     uint32_t numGen = 0) {
    if (!animCurrent(AnimSlot::Name, gen) || !text) return false;

    stopNameTeeter();
    nameStaticValid = false;
    uint16_t startCol = 0, endCol = 0;
    board->getDisplayExtent(z, startCol, endCol);
    const uint16_t zCols = zoneWidthCols(z);
    const uint16_t tCols = buildNameColumns(text, teeter.colBuf, NAME_COL_BUF);
    if (zCols == 0) return animCurrent(AnimSlot::Name, gen);

    const uint8_t resolved = resolveEffect(effect);
    teeter.zone = z;
    teeter.zoneStart = startCol;
    teeter.zoneCols = zCols;
    teeter.textCols = tCols;

    const bool dual = (numZone >= 0);

    auto dualStillOk = [&]() -> bool {
      if (!animCurrent(AnimSlot::Name, gen)) return false;
      if (dual && !animCurrent(AnimSlot::Num, numGen)) return false;
      return true;
    };

    auto fadeNameIn = [&](int16_t nameOrigin) -> bool {
      const uint8_t target = effectiveIntensity();
      if (dual) {
        board->setIntensity(0);
      } else {
        setNameModulesIntensity(0);
      }
      const uint16_t frameMs = static_cast<uint16_t>(transitionFrameMs() + 4);
      for (uint8_t i = 0; i <= target; i++) {
        if (!dualStillOk()) return false;
        if (dual) board->setIntensity(i);
        else setNameModulesIntensity(i);
        const uint32_t t0 = millis();
        while (static_cast<uint32_t>(millis() - t0) < frameMs) {
          if (!dualStillOk()) return false;
          if (dual && !tickNumberKeepName(numZone, numGen, nameOrigin)) return false;
          pollDuringAnim();
        }
      }
      board->setIntensity(effectiveIntensity());
      return dualStillOk();
    };

    auto scrollNameTo = [&](int16_t from, int16_t to, int16_t step) -> bool {
      const uint16_t frameMs = transitionFrameMs();
      board->displayClear(z);
      for (int16_t origin = from;;) {
        if (!dualStillOk()) return false;
        blitNameWindow(z, startCol, zCols, teeter.colBuf, tCols, origin);
        if (dual && !tickNumberKeepName(numZone, numGen, origin)) return false;
        if (origin == to) break;
        int16_t next = static_cast<int16_t>(origin + step);
        if (step > 0 && next > to) next = to;
        if (step < 0 && next < to) next = to;
        origin = next;
        const uint32_t t0 = millis();
        while (static_cast<uint32_t>(millis() - t0) < frameMs) {
          if (!dualStillOk()) return false;
          if (dual && !tickNumberKeepName(numZone, numGen, origin)) return false;
          pollDuringAnim();
        }
      }
      return dualStillOk();
    };

    auto settleNumber = [&](int16_t nameOrigin) -> bool {
      if (!dual) return true;
      return finishNumberKeepName(numZone, numGen, nameOrigin);
    };

    auto runDualParola = [&]() -> bool {
      const textEffect_t inEff = toParolaEffect(resolved);
      uint16_t spd = parolaScrollSpeed(30);
      if (resolved == ShowMode::ANIMATION) spd = 4;
      board->displayClear(z);
      board->displayClear(numZone);
      board->setCharSpacing(numZone, 1);
      board->displayZoneText(z, text, PA_CENTER, spd, 0, inEff, PA_NO_EFFECT);
      board->displayZoneText(numZone, numText, PA_CENTER, spd, 0, inEff, PA_NO_EFFECT);
      board->displayReset(z);
      board->displayReset(numZone);
      while (!board->getZoneStatus(z) || !board->getZoneStatus(numZone)) {
        if (!dualStillOk()) return false;
        board->displayAnimate();
        pollDuringAnim();
      }
      paintName(z, text);
      return dualStillOk();
    };

    // Oversized names: transition into teeter start (offset 0), then bounce.
    if (tCols > zCols) {
      if (resolved == ShowMode::SMOTH) {
        if (dual && !startNumberInAnim(numZone, numText, resolved, numGen)) return false;
        blitNameWindow(z, startCol, zCols, teeter.colBuf, tCols, 0);
        if (!fadeNameIn(0)) return false;
        if (!settleNumber(0)) return false;
        startNameTeeter(z, text);
        return dualStillOk();
      }
      if (resolved == ShowMode::LEFT || resolved == ShowMode::RIGHT) {
        if (dual && !startNumberInAnim(numZone, numText, resolved, numGen)) return false;
        const int16_t stepCols = static_cast<int16_t>(transitionStepCols());
        int16_t from = static_cast<int16_t>(-static_cast<int16_t>(zCols));
        int16_t step = stepCols;
        if (resolved == ShowMode::RIGHT) {
          from = static_cast<int16_t>(tCols);
          step = static_cast<int16_t>(-stepCols);
        }
        if (!scrollNameTo(from, 0, step)) return false;
        if (!settleNumber(0)) return false;
        startNameTeeter(z, text);
        return dualStillOk();
      }
      if (dual) {
        if (!runDualParola()) return false;
      }
      startNameTeeter(z, text);
      return dualStillOk();
    }

    const int16_t finalOrigin = static_cast<int16_t>(
        -((static_cast<int16_t>(zCols) - static_cast<int16_t>(tCols)) / 2));

    if (resolved == ShowMode::FREEZE) {
      if (dual && !startNumberInAnim(numZone, numText, resolved, numGen)) return false;
      paintNameStatic(z, teeter.colBuf, tCols);
      return settleNumber(finalOrigin) && dualStillOk();
    }

    // Smooth = fade in name (+ whole board when number joins)
    if (resolved == ShowMode::SMOTH) {
      if (dual && !startNumberInAnim(numZone, numText, resolved, numGen)) return false;
      board->displayClear(z);
      blitNameWindow(z, startCol, zCols, teeter.colBuf, tCols, finalOrigin);
      nameStaticValid = true;
      nameStaticOrigin = finalOrigin;
      if (!fadeNameIn(finalOrigin)) return false;
      return settleNumber(finalOrigin) && dualStillOk();
    }

    // Only Left/Right are horizontal scrolls
    if (resolved != ShowMode::LEFT && resolved != ShowMode::RIGHT) {
      if (dual) return runDualParola();
      if (!runEffectAnim(z, text, resolved, 30, 0, gen, AnimSlot::Name)) return false;
      paintName(z, text);
      return animCurrent(AnimSlot::Name, gen);
    }

    if (dual && !startNumberInAnim(numZone, numText, resolved, numGen)) return false;
    const int16_t stepCols = static_cast<int16_t>(transitionStepCols());
    int16_t from = static_cast<int16_t>(-static_cast<int16_t>(zCols));
    int16_t step = stepCols;
    if (resolved == ShowMode::RIGHT) {
      from = static_cast<int16_t>(tCols);
      step = static_cast<int16_t>(-stepCols);
    }
    if (!scrollNameTo(from, finalOrigin, step)) return false;
    nameStaticValid = true;
    nameStaticOrigin = finalOrigin;
    if (!settleNumber(finalOrigin)) return false;
    return dualStillOk();
  }

  void tickNameTeeter() {
    if (!teeter.active || teeter.overflow == 0) return;
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - teeter.nextMs) < 0) return;

    if (teeter.pausing) {
      teeter.pausing = false;
      teeter.nextMs = now + teeterFrameMs();
      return;
    }

    teeter.offset = static_cast<int16_t>(teeter.offset + teeter.dir);
    if (teeter.offset >= static_cast<int16_t>(teeter.overflow)) {
      teeter.offset = static_cast<int16_t>(teeter.overflow);
      teeter.dir = -1;
      teeter.pausing = true;
      teeter.nextMs = now + TEETER_END_PAUSE_MS;
    } else if (teeter.offset <= 0) {
      teeter.offset = 0;
      teeter.dir = 1;
      teeter.pausing = true;
      teeter.nextMs = now + TEETER_END_PAUSE_MS;
    } else {
      teeter.nextMs = now + teeterFrameMs();
    }
    blitTeeterFrame();
  }

  void paintNameOnly() {
    const int8_t nz = nameZoneId();
    if (nz < 0) return;
    const Counter& c = Counters::getConst(Counters::activeIndex());
    strncpy(nameBuf, c.name, NAME_MAX_LEN);
    nameBuf[NAME_MAX_LEN] = '\0';
    paintName(nz, nameBuf);
  }

  void paintContent() {
    const uint8_t src = Counters::activeIndex();
    const Counter& c = Counters::getConst(src);

    strncpy(nameBuf, c.name, NAME_MAX_LEN);
    nameBuf[NAME_MAX_LEN] = '\0';
    formatCount(c.count);

    stopNameTeeter();
    board->displayClear();

    const int8_t nz = nameZoneId();
    const int8_t cz = numberZone();

    // Settle the number zone fully first. If we paint/teeter the name first,
    // we skip Parola animate (so it wouldn't overwrite name) and the count
    // never makes it to the panels.
    if (cz >= 0) {
      board->setCharSpacing(cz, 1);
      board->displayZoneText(cz, numBuf, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
      board->displayReset(cz);
      while (!board->getZoneStatus(cz)) {
        board->displayAnimate();
      }
    }

    if (nz >= 0) {
      paintName(nz, nameBuf);
    }
  }

  void applyBlankState() {
    if (!inited || !board) return;
    scrollerOn = false;

    if (!Counters::blanked()) {
      board->displayShutdown(false);
      board->setIntensity(Counters::intensity());
      paintContent();
      return;
    }

    const uint8_t pct = Counters::blankBrightnessPercent();
    if (pct == 0) {
      board->displayClear();
      board->displayShutdown(true);
      return;
    }

    board->displayShutdown(false);
    board->setIntensity(percentToIntensity(pct));
    paintContent();
  }

  // After number-only anims: keep blank intensity without clearing/repainting
  // (paintContent would flash the name).
  void restoreBlankAfterAnim() {
    if (!inited || !board) return;
    if (!Counters::blanked()) {
      board->setIntensity(Counters::intensity());
      return;
    }
    const uint8_t pct = Counters::blankBrightnessPercent();
    if (pct == 0) {
      board->displayClear();
      board->displayShutdown(true);
      return;
    }
    board->displayShutdown(false);
    board->setIntensity(percentToIntensity(pct));
    restoreNameBlit();
  }

  // Returns false if superseded by a newer animation
  bool runZoneAnim(uint8_t zone, const char* text, textEffect_t inEff, textEffect_t outEff,
                   uint16_t speed, uint16_t pauseMs, uint32_t gen, AnimSlot slot) {
    if (!animCurrent(slot, gen)) return false;

    board->displayClear(zone);
    board->displayZoneText(zone, text, PA_CENTER, speed, pauseMs, inEff, outEff);
    board->displayReset(zone);
    while (!board->getZoneStatus(zone)) {
      if (!animCurrent(slot, gen)) return false;
      board->displayAnimate();
      // Parola rewrites every zone; put the name pixels back after number anims.
      if (slot == AnimSlot::Num) restoreNameBlit();
      pollDuringAnim();
    }
    if (slot == AnimSlot::Num) restoreNameBlit();
    return animCurrent(slot, gen);
  }

  bool runEffectAnim(uint8_t zone, const char* text, uint8_t effect,
                     uint16_t speed, uint16_t pauseMs, uint32_t gen, AnimSlot slot) {
    if (!animCurrent(slot, gen)) return false;

    const uint8_t resolved = resolveEffect(effect);
    const textEffect_t inEff = toParolaEffect(resolved);
    if (inEff == PA_PRINT) {
      board->displayClear(zone);
      board->displayZoneText(zone, text, PA_CENTER, 0, pauseMs ? pauseMs : 80, PA_PRINT, PA_NO_EFFECT);
      board->displayReset(zone);
      while (!board->getZoneStatus(zone)) {
        if (!animCurrent(slot, gen)) return false;
        board->displayAnimate();
        if (slot == AnimSlot::Num) restoreNameBlit();
        pollDuringAnim();
      }
      if (slot == AnimSlot::Num) restoreNameBlit();
      return animCurrent(slot, gen);
    }
    uint16_t spd = parolaScrollSpeed(speed);
    if (resolved == ShowMode::ANIMATION) {
      spd = 4;
    }
    return runZoneAnim(zone, text, inEff, PA_NO_EFFECT, spd, pauseMs, gen, slot);
  }

  void playBootAnimation() {
    board->displayShutdown(false);
    board->setIntensity(effectiveIntensity());
    board->displayClear();

    const int8_t z = primaryAnimZone();
    if (z < 0) return;

    const uint32_t gen = beginAnim(AnimSlot::Name);
    beginAnim(AnimSlot::Num);
    runZoneAnim(z, "****", PA_WIPE_CURSOR, PA_NO_EFFECT, 20, 180, gen, AnimSlot::Name);
    if (animCurrent(AnimSlot::Name, gen)) {
      runZoneAnim(z, "****", PA_NO_EFFECT, PA_WIPE, 20, 0, gen, AnimSlot::Name);
    }
    if (animCurrent(AnimSlot::Name, gen)) {
      board->displayClear();
    }
  }

  bool playFireworks(int32_t count, uint32_t gen) {
    MD_MAX72XX* mx = board->getGraphicObject();
    if (!mx) return false;

    // Number strip when present; otherwise the whole (name-only) chain
    const uint16_t col0 = (MODULES_NUMBER > 0) ? (MODULES_NAME * COLS_PER_MOD) : 0;
    const uint16_t colN = MODULES_PER_BOARD * COLS_PER_MOD;
    if (colN <= col0) return animCurrent(AnimSlot::Num, gen);

    const uint8_t clearFrom = (MODULES_NUMBER > 0) ? MODULES_NAME : 0;
    const uint8_t base = effectiveIntensity();

    board->displayShutdown(false);
    board->setIntensity(min<uint8_t>(15, static_cast<uint8_t>(base + 4)));

    for (uint8_t frame = 0; frame < 48; frame++) {
      if (!animCurrent(AnimSlot::Num, gen)) return false;
      mx->clear(clearFrom, MODULES_PER_BOARD - 1);

      const uint8_t sparks = 10 + (frame % 8);
      for (uint8_t s = 0; s < sparks; s++) {
        const uint16_t col = col0 + (esp_random() % (colN - col0));
        const uint8_t row = esp_random() % 8;
        mx->setPoint(row, col, true);
        if (row > 0) mx->setPoint(row - 1, col, true);
        if (row < 7) mx->setPoint(row + 1, col, true);
        if (col > col0) mx->setPoint(row, col - 1, true);
        if (col + 1 < colN) mx->setPoint(row, col + 1, true);
      }
      mx->update();
      delay(28);
      pollDuringAnim();
    }

    if (!animCurrent(AnimSlot::Num, gen)) return false;

    formatCount(count);
    const int8_t z = primaryAnimZone();
    // Leave name zone alone — only flash/invert the number zone.
    board->setIntensity(base);
    for (uint8_t i = 0; i < 3; i++) {
      if (!animCurrent(AnimSlot::Num, gen)) return false;
      if (z >= 0) {
        board->displayZoneText(z, numBuf, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
        board->displayAnimate();
        restoreNameBlit();
        board->setInvert(z, true);
      }
      delay(80);
      pollDuringAnim();
      if (!animCurrent(AnimSlot::Num, gen)) {
        if (z >= 0) board->setInvert(z, false);
        return false;
      }
      if (z >= 0) board->setInvert(z, false);
      delay(80);
      pollDuringAnim();
    }

    board->setIntensity(base);
    return animCurrent(AnimSlot::Num, gen);
  }

  bool playCountAnim(int32_t newCount, uint8_t effect, uint32_t gen) {
    const int8_t z = primaryAnimZone();
    if (z < 0) return animCurrent(AnimSlot::Num, gen);

    // Number zone only — do not redraw/restart name teeter or transitions.
    formatCount(newCount);
    return runEffectAnim(z, numBuf, effect, 25, 100, gen, AnimSlot::Num);
  }
}

namespace Display {

uint8_t primaryBoard() { return 0; }

// Forward — used by idleShowNext / stopManualScroller
void haltScrollerEngine();

void begin() {
  SPI.begin(PIN_CLK, -1, PIN_DIN, -1);

  if (PIN_CS < 0) {
    Serial.println(F("[display] no CS configured"));
    return;
  }

  board = new MD_Parola(HW, static_cast<uint8_t>(PIN_CS), MODULES_PER_BOARD);
  board->begin(ZONE_COUNT);

  if (MODULES_NAME > 0 && MODULES_NUMBER > 0) {
    board->setZone(0, 0, MODULES_NAME - 1);
    board->setZone(1, MODULES_NAME, MODULES_PER_BOARD - 1);
  } else {
    board->setZone(0, 0, MODULES_PER_BOARD - 1);
  }

  board->setIntensity(effectiveIntensity());
  board->displayClear();
  inited = true;

  playBootAnimation();
  refreshAll();
}

void loop() {
  if (!inited || !board) return;

  if (flashOn && millis() >= flashUntil) {
    flashOn = false;
    board->setInvert(false);
    applyBlankState();
  }

  if (!displayVisible()) return;

  if (scrollerOn) {
    board->displayAnimate();
    if (board->getZoneStatus(scrollerZone)) {
      board->displayReset(scrollerZone);
    }
    return;
  }

  if (teeter.active) {
    // Direct column blit for name; avoid Parola animate (it would overwrite name).
    // Number zone pixels remain in the MAX72XX buffer across updates.
    tickNameTeeter();
  } else {
    board->displayAnimate();
  }
}

void refreshAll() {
  applyBlankState();
}

void refresh(uint8_t /*index*/) {
  applyBlankState();
}

void applyIntensity() {
  if (!inited || !board) return;
  if (Counters::blanked()) {
    applyBlankState();
  } else {
    board->setIntensity(Counters::intensity());
  }
}

void applyBlanked() {
  applyBlankState();
}

void flashActive(uint8_t /*index*/) {
  if (!inited || !board) return;
  if (!displayVisible()) return;

  applyBlankState();
  board->setInvert(true);
  flashOn = true;
  flashUntil = millis() + 250;
}

void onCountIncreased(int32_t newCount, uint8_t effect) {
  scrollerOn = false;
  if (!inited || !board || !displayVisible()) {
    applyBlankState();
    return;
  }

  // Number-only anim gen — does not cancel name transitions/teeter.
  const uint32_t gen = beginAnim(AnimSlot::Num);
  board->displayShutdown(false);
  board->setIntensity(effectiveIntensity());

  bool ok;
  if (isMilestone(newCount)) {
    ok = playFireworks(newCount, gen);
  } else {
    ok = playCountAnim(newCount, resolveEffect(effect), gen);
  }
  if (ok) {
    restoreBlankAfterAnim();
  }
}

void onCountDecreased(int32_t newCount, uint8_t effect) {
  scrollerOn = false;
  if (!inited || !board || !displayVisible()) {
    applyBlankState();
    return;
  }

  const uint32_t gen = beginAnim(AnimSlot::Num);
  board->displayShutdown(false);
  board->setIntensity(effectiveIntensity());
  if (playCountAnim(newCount, resolveEffect(effect), gen)) {
    restoreBlankAfterAnim();
  }
}

void idleShowNext(int32_t /*count*/, uint8_t effect) {
  // If marquee was running, kill it hard so the enter transition owns the panel.
  if (scrollerOn) {
    haltScrollerEngine();
  }
  if (!inited || !board || !displayVisible()) {
    applyBlankState();
    return;
  }

  board->displayShutdown(false);
  board->setIntensity(effectiveIntensity());

  const uint8_t eff = resolveEffect(effect);
  const int8_t nz = nameZoneId();
  const int8_t cz = numberZone();
  const Counter& c = Counters::getConst(Counters::activeIndex());

  formatCount(c.count);
  strncpy(nameBuf, c.name, NAME_MAX_LEN);
  nameBuf[NAME_MAX_LEN] = '\0';

  const uint32_t nameGen = beginAnim(AnimSlot::Name);
  const uint32_t numGen = beginAnim(AnimSlot::Num);

  if (nz >= 0 && cz >= 0) {
    // Name + number transition together with the same effect.
    if (!runNameInAnim(nz, nameBuf, eff, nameGen, cz, numBuf, numGen)) return;
  } else if (cz >= 0) {
    if (!runEffectAnim(cz, numBuf, eff, 30, 0, numGen, AnimSlot::Num)) return;
  } else if (nz >= 0) {
    if (!runNameInAnim(nz, nameBuf, eff, nameGen)) return;
  }

  if (Counters::blanked()) {
    restoreBlankAfterAnim();
  } else {
    board->displayShutdown(false);
    board->setIntensity(effectiveIntensity());
  }
}

void selectShow(uint8_t effect) {
  idleShowNext(0, effect);
}

bool scrollerActive() { return scrollerOn; }

// Halt marquee and leave Parola zones idle so loop() won't keep scrolling.
void haltScrollerEngine() {
  scrollerOn = false;
  stopNameTeeter();
  nameStaticValid = false;
  beginAnim(AnimSlot::Name);
  beginAnim(AnimSlot::Num);
  if (!inited || !board) return;

  board->displayShutdown(false);
  board->setIntensity(effectiveIntensity());
  board->displayClear();

  auto settleZoneIdle = [&](int8_t z) {
    if (z < 0) return;
    board->displayClear(z);
    board->displayZoneText(z, " ", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
    board->displayReset(z);
    const uint32_t t0 = millis();
    while (!board->getZoneStatus(z) && (millis() - t0) < 250) {
      board->displayAnimate();
    }
    board->displayClear(z);
  };
  settleZoneIdle(nameZoneId());
  settleZoneIdle(numberZone());
}

void stopManualScroller(bool restore) {
  if (!scrollerOn) return;
  haltScrollerEngine();
  if (!inited || !board) return;
  if (restore) applyBlankState();
}

void startManualScroller(uint8_t effect) {
  if (!inited || !board) return;
  if (!displayVisible()) {
    applyBlankState();
    return;
  }

  stopNameTeeter();
  nameStaticValid = false;
  beginAnim(AnimSlot::Name);
  beginAnim(AnimSlot::Num);

  const char* msg = Counters::scrollMessage();
  if (msg && msg[0]) {
    strncpy(scrollerBuf, msg, SCROLLER_MAX_LEN);
    scrollerBuf[SCROLLER_MAX_LEN] = '\0';
  } else {
    strncpy(scrollerBuf, " ", sizeof(scrollerBuf));
  }

  const uint8_t resolved = resolveEffect(effect);
  scrollerDir = (resolved == ShowMode::RIGHT) ? PA_SCROLL_RIGHT : PA_SCROLL_LEFT;

  const int8_t nz = nameZoneId();
  const int8_t cz = numberZone();
  if (nz < 0) return;

  scrollerZone = static_cast<uint8_t>(nz);
  scrollerOn = true;

  board->displayShutdown(false);
  board->setIntensity(effectiveIntensity());
  board->displayClear();

  if (cz >= 0) {
    board->displayClear(cz);
  }

  const uint16_t spd = parolaScrollSpeed(30);
  board->setCharSpacing(scrollerZone, 1);
  board->displayZoneText(scrollerZone, scrollerBuf, PA_LEFT, spd, 0, scrollerDir, scrollerDir);
  board->displayReset(scrollerZone);
}

}  // namespace Display
