#include "display.h"
#include "counters.h"
#include "show_mode.h"
#include "wiz_remote.h"
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

  // Prevent remote handlers from nesting another blocking anim (stack overflow on C3).
  bool pollingInputs = false;
  bool animBusy = false;

  enum class PendingKind : uint8_t { None, CounterEnter, CountInc, CountDec };
  struct PendingAnim {
    PendingKind kind = PendingKind::None;
    uint8_t effect = 0;
    int32_t count = 0;
  } pendingAnim;

  void cancelInFlightAnims() {
    beginAnim(AnimSlot::Name);
    beginAnim(AnimSlot::Num);
  }

  void pollDuringAnim() {
    if (pollingInputs) {
      yield();
      return;
    }
    pollingInputs = true;
    WizRemote::loop();
    RemoteMap::loop();
    pollingInputs = false;
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
      case ShowMode::LASER_LEFT: return PA_WIPE_CURSOR;
      case ShowMode::LASER_RIGHT: return PA_SLICE;
      case ShowMode::LASER_UP: return PA_SCAN_VERT;
      case ShowMode::LASER_DOWN: return PA_SCAN_VERTX;
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

  // Transition scroll: ms between column steps (lower = faster overall)
  uint16_t transitionFrameMs() {
    uint8_t s = Counters::scrollSpeed();
    if (s < 1) s = 1;
    if (s > 10) s = 10;
    // ~26ms at 1 → ~8ms at 10
    return static_cast<uint16_t>(28 - ((s - 1) * 20) / 9);
  }

  // Horizontal scroll progress per frame: 1 at speed 1 → 3 at speed 10 (up to 3×)
  uint8_t transitionStride() {
    uint8_t s = Counters::scrollSpeed();
    if (s < 1) s = 1;
    if (s > 10) s = 10;
    return static_cast<uint8_t>(1 + ((s - 1) * 2) / 9);
  }

  // Parola effect speed (lower = faster); scale with scrollSpeed (transitions)
  uint16_t parolaScrollSpeed(uint16_t base) {
    uint8_t s = Counters::scrollSpeed();
    if (s < 1) s = 1;
    if (s > 10) s = 10;
    // Gentler than linear: keep mid speeds from feeling rushed
    const int scaled = (static_cast<int>(base) * 6) / static_cast<int>(s);
    if (scaled < 6) return 6;
    if (scaled > 80) return 80;
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

  // Manual marquee / static message screen (name + optional bottom)
  bool scrollerOn = false;
  bool scrollerPassDone = false;
  bool scrollerTopScrolls = false;
  bool scrollerBottomScrolls = false;
  bool scrollerTopPassLatched = false;  // finished one pass (don't restart)
  bool scrollerBotPassLatched = false;
  uint8_t scrollerZone = 0;
  uint8_t scrollerBottomZone = 0;
  bool scrollerHasBottom = false;
  char scrollerBuf[SCROLLER_MAX_LEN + 1] = "";
  char scrollerBottomBuf[SCROLLER_BOTTOM_MAX_LEN + 1] = "";
  textEffect_t scrollerDir = PA_SCROLL_LEFT;
  textEffect_t scrollerBottomDir = PA_SCROLL_LEFT;

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

  // Build column buffer with 1px between glyphs (including spaces).
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
      if (i + 1 < len && n < outMax) {
        out[n++] = 0;
      }
    }
    return n;
  }

  // origin = source column of the leftmost visible pixel (negative pads left).
  // doUpdate=false to batch multiple zone blits before one mx->update().
  void blitZoneColumns(uint16_t zoneStart, uint16_t zoneCols, const uint8_t* colBuf,
                       uint16_t textCols, int16_t origin, bool flipLR, bool flipUD,
                       bool doUpdate) {
    MD_MAX72XX* mx = board->getGraphicObject();
    if (!mx || zoneCols == 0) return;

    const uint16_t zoneEnd = zoneStart + zoneCols - 1;
    for (uint16_t i = 0; i < zoneCols; i++) {
      const uint16_t destI = flipLR ? (zoneCols - 1 - i) : i;
      const int32_t src = static_cast<int32_t>(origin) + static_cast<int32_t>(i);
      uint8_t v =
          (src >= 0 && src < static_cast<int32_t>(textCols)) ? colBuf[src] : 0;
      if (flipUD) {
        v = static_cast<uint8_t>(((v * 0x0802u & 0x22110u) | (v * 0x8020u & 0x88440u)) * 0x10101u >> 16);
      }
      mx->setColumn(zoneEnd - destI, v);
    }
    if (doUpdate) mx->update();
  }

  void blitNameWindow(uint8_t /*zone*/, uint16_t zoneStart, uint16_t zoneCols,
                      const uint8_t* colBuf, uint16_t textCols, int16_t origin) {
    blitZoneColumns(zoneStart, zoneCols, colBuf, textCols, origin,
                    NAME_ZONE_FLIP_LR, NAME_ZONE_FLIP_UD, true);
  }

  // Capture zone columns in the same logical order blitZoneColumns uses.
  void snapshotZone(uint16_t zoneStart, uint16_t zoneCols, uint8_t* out, bool flipLR) {
    MD_MAX72XX* mx = board->getGraphicObject();
    if (!mx || zoneCols == 0 || !out) return;
    const uint16_t zoneEnd = zoneStart + zoneCols - 1;
    for (uint16_t i = 0; i < zoneCols; i++) {
      const uint16_t destI = flipLR ? (zoneCols - 1 - i) : i;
      out[i] = mx->getColumn(zoneEnd - destI);
    }
  }

  // Push: old and new share one origin timeline (incoming replaces outgoing).
  void blitZonePush(uint16_t zoneStart, uint16_t zoneCols,
                    const uint8_t* oldBuf, uint16_t oldCols, int16_t oldOrigin,
                    const uint8_t* newBuf, uint16_t newCols, int16_t newOrigin,
                    bool flipLR, bool flipUD, bool doUpdate) {
    MD_MAX72XX* mx = board->getGraphicObject();
    if (!mx || zoneCols == 0) return;
    const uint16_t zoneEnd = zoneStart + zoneCols - 1;
    for (uint16_t i = 0; i < zoneCols; i++) {
      const uint16_t destI = flipLR ? (zoneCols - 1 - i) : i;
      const int32_t srcN = static_cast<int32_t>(newOrigin) + static_cast<int32_t>(i);
      const int32_t srcO = static_cast<int32_t>(oldOrigin) + static_cast<int32_t>(i);
      uint8_t v = 0;
      if (srcN >= 0 && srcN < static_cast<int32_t>(newCols) && newBuf) {
        v = newBuf[srcN];
        if (flipUD) {
          v = static_cast<uint8_t>(((v * 0x0802u & 0x22110u) | (v * 0x8020u & 0x88440u)) * 0x10101u >> 16);
        }
      } else if (srcO >= 0 && srcO < static_cast<int32_t>(oldCols) && oldBuf) {
        v = oldBuf[srcO];  // already on-screen orientation
      }
      mx->setColumn(zoneEnd - destI, v);
    }
    if (doUpdate) mx->update();
  }

  // Exit with the same Parola effect used for enter (toParolaEffect).
  // resolved = already resolveEffect()'d.
  bool scrollDisplayedOff(uint8_t resolved, uint32_t nameGen, uint32_t numGen,
                          bool fromScroller) {
    const int8_t nz = nameZoneId();
    const int8_t cz = numberZone();
    if (nz < 0 && cz < 0) return true;

    auto stillOk = [&]() -> bool {
      if (nz >= 0 && !animCurrent(AnimSlot::Name, nameGen)) return false;
      if (cz >= 0 && !animCurrent(AnimSlot::Num, numGen)) return false;
      return true;
    };

    if (resolved == ShowMode::FREEZE || fromScroller) {
      // FREEZE: instant clear.
      // Scroller: never PA_PRINT the message — that flashes it back after scroll-off.
      if (!stillOk()) return false;
      board->displayClear();
      return stillOk();
    }

    // Scoreboard: text currently on screen (Parola out needs the string).
    char top[SCROLLER_MAX_LEN + 1];
    char bot[SCROLLER_BOTTOM_MAX_LEN + 1];
    {
      const Counter& c = Counters::getConst(Counters::activeIndex());
      strncpy(top, c.name, sizeof(top) - 1);
      top[sizeof(top) - 1] = '\0';
      formatCount(c.count);
      strncpy(bot, numBuf, sizeof(bot) - 1);
      bot[sizeof(bot) - 1] = '\0';
    }
    if (!top[0]) strncpy(top, " ", sizeof(top));
    if (!bot[0]) strncpy(bot, " ", sizeof(bot));

    const textEffect_t outEff = toParolaEffect(resolved);
    uint16_t spd = parolaScrollSpeed(40);
    if (resolved == ShowMode::ANIMATION) spd = 6;

    // Instant print then out-effect — same Parola engine / mapping as enter.
    if (nz >= 0) {
      board->setCharSpacing(static_cast<uint8_t>(nz), 1);
      board->displayZoneText(static_cast<uint8_t>(nz), top, PA_CENTER, spd, 0, PA_PRINT,
                             outEff);
      board->displayReset(static_cast<uint8_t>(nz));
    }
    if (cz >= 0) {
      board->setCharSpacing(static_cast<uint8_t>(cz), 1);
      board->displayZoneText(static_cast<uint8_t>(cz), bot, PA_CENTER, spd, 0, PA_PRINT,
                             outEff);
      board->displayReset(static_cast<uint8_t>(cz));
    }

    while ((nz >= 0 && !board->getZoneStatus(static_cast<uint8_t>(nz))) ||
           (cz >= 0 && !board->getZoneStatus(static_cast<uint8_t>(cz)))) {
      if (!stillOk()) return false;
      board->displayAnimate();
      pollDuringAnim();
    }
    return stillOk();
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
    uint16_t spd = parolaScrollSpeed(40);
    if (resolved == ShowMode::ANIMATION) spd = 6;
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
  // Dual LEFT/RIGHT: shared timeline — both center together (wide panel ≈ 2× travel).
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

    const bool dual = (numZone >= 0 && numText);

    auto stillOk = [&]() -> bool {
      if (!animCurrent(AnimSlot::Name, gen)) return false;
      if (dual && !animCurrent(AnimSlot::Num, numGen)) return false;
      return true;
    };

    auto lerpOrigin = [](int16_t a, int16_t b, uint16_t i, uint16_t n) -> int16_t {
      if (n == 0) return b;
      return static_cast<int16_t>(
          a + (static_cast<int32_t>(b - a) * static_cast<int32_t>(i)) / static_cast<int32_t>(n));
    };

    // LEFT/RIGHT: old content exits while new content enters on one timeline.
    auto scrollNameWithNumber = [&](int16_t from, int16_t to, int16_t /*step*/) -> bool {
      const bool slideLeft = (to >= from);
      const uint16_t frameMs = transitionFrameMs();

      constexpr uint16_t NUM_COL_BUF = 64;
      uint8_t numColBuf[NUM_COL_BUF];
      uint8_t nameOld[MODULES_NAME * COLS_PER_MOD];
      uint8_t numOld[MODULES_NUMBER * COLS_PER_MOD];

      snapshotZone(startCol, zCols, nameOld, NAME_ZONE_FLIP_LR);

      uint16_t nStart = 0, nEnd = 0;
      uint16_t nZoneCols = 0;
      uint16_t nTextCols = 0;
      uint16_t nOldCols = 0;
      int16_t nFrom = 0, nTo = 0;
      int16_t nOldFrom = 0, nOldTo = 0;

      if (dual) {
        board->getDisplayExtent(static_cast<uint8_t>(numZone), nStart, nEnd);
        nZoneCols = zoneWidthCols(static_cast<uint8_t>(numZone));
        if (nZoneCols > sizeof(numOld)) nZoneCols = sizeof(numOld);
        snapshotZone(nStart, nZoneCols, numOld, false);
        nOldCols = nZoneCols;
        nTextCols = buildNameColumns(numText, numColBuf, NUM_COL_BUF);
        nTo = (nTextCols >= nZoneCols)
                  ? 0
                  : static_cast<int16_t>(
                        -((static_cast<int16_t>(nZoneCols) - static_cast<int16_t>(nTextCols)) / 2));
        nFrom = slideLeft ? static_cast<int16_t>(-static_cast<int16_t>(nZoneCols))
                          : static_cast<int16_t>(nTextCols);
        nOldFrom = 0;
        nOldTo = slideLeft ? static_cast<int16_t>(nOldCols)
                           : static_cast<int16_t>(-static_cast<int16_t>(nOldCols));
      }

      const int16_t oldFrom = 0;
      const int16_t oldTo =
          slideLeft ? static_cast<int16_t>(zCols) : static_cast<int16_t>(-static_cast<int16_t>(zCols));

      // Duration follows the longer travel so both panels stay in sync.
      const uint16_t nameDist = static_cast<uint16_t>(from > to ? (from - to) : (to - from));
      const uint16_t nameOldDist =
          static_cast<uint16_t>(oldFrom > oldTo ? (oldFrom - oldTo) : (oldTo - oldFrom));
      const uint16_t numDist =
          dual ? static_cast<uint16_t>(nFrom > nTo ? (nFrom - nTo) : (nTo - nFrom)) : 0;
      const uint16_t numOldDist =
          dual ? static_cast<uint16_t>(nOldFrom > nOldTo ? (nOldFrom - nOldTo) : (nOldTo - nOldFrom))
               : 0;
      uint16_t frames = nameDist;
      if (nameOldDist > frames) frames = nameOldDist;
      if (numDist > frames) frames = numDist;
      if (numOldDist > frames) frames = numOldDist;
      if (frames == 0) frames = 1;

      auto blitBoth = [&](int16_t oldO, int16_t newO, int16_t nOldO, int16_t nNewO) {
        blitZonePush(startCol, zCols, nameOld, zCols, oldO, teeter.colBuf, tCols, newO,
                     NAME_ZONE_FLIP_LR, NAME_ZONE_FLIP_UD, !dual);
        if (dual) {
          blitZonePush(nStart, nZoneCols, numOld, nOldCols, nOldO, numColBuf, nTextCols, nNewO,
                       false, false, true);
        }
      };

      blitBoth(oldFrom, from, dual ? nOldFrom : 0, dual ? nFrom : 0);

      const uint8_t stride = transitionStride();
      for (uint16_t i = 0; i < frames; ) {
        if (!stillOk()) return false;
        uint16_t next = static_cast<uint16_t>(i + stride);
        if (next > frames) next = frames;
        i = next;
        blitBoth(lerpOrigin(oldFrom, oldTo, i, frames), lerpOrigin(from, to, i, frames),
                 dual ? lerpOrigin(nOldFrom, nOldTo, i, frames) : 0,
                 dual ? lerpOrigin(nFrom, nTo, i, frames) : 0);
        const uint32_t t0 = millis();
        while (static_cast<uint32_t>(millis() - t0) < frameMs) {
          if (!stillOk()) return false;
          pollDuringAnim();
        }
      }
      return stillOk();
    };

    auto paintNumberParola = [&]() -> bool {
      if (!dual || !numText) return true;
      board->setCharSpacing(numZone, 1);
      board->displayClear(numZone);
      board->displayZoneText(numZone, numText, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
      board->displayReset(numZone);
      while (!board->getZoneStatus(numZone)) {
        if (!stillOk()) return false;
        board->displayAnimate();
        // Parola may touch the name zone — keep the faded/static name pixels.
        blitNameWindow(z, startCol, zCols, teeter.colBuf, tCols,
                       nameStaticValid ? nameStaticOrigin : 0);
        pollDuringAnim();
      }
      return stillOk();
    };

    auto fadeNameWithNumber = [&](int16_t nameOrigin) -> bool {
      const uint8_t target = effectiveIntensity();
      const uint16_t frameMs = static_cast<uint16_t>(transitionFrameMs() + 4);

      // Number uses the same Parola PA_PRINT path as paintContent — custom column
      // blit centered differently and jumped ~1px when loop() called displayAnimate.
      nameStaticValid = false;
      if (dual) {
        board->setCharSpacing(numZone, 1);
        board->displayClear(numZone);
        board->displayZoneText(numZone, numText, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
        board->displayReset(numZone);
        while (!board->getZoneStatus(numZone)) {
          if (!stillOk()) return false;
          board->displayAnimate();
          pollDuringAnim();
        }
      }

      setNameModulesIntensity(0);
      blitNameWindow(z, startCol, zCols, teeter.colBuf, tCols, nameOrigin);

      for (uint8_t level = 0; level <= target; level++) {
        if (!stillOk()) return false;
        setNameModulesIntensity(level);
        // Keep name pixels; do not call displayAnimate (would fight the blit).
        const uint32_t t0 = millis();
        while (static_cast<uint32_t>(millis() - t0) < frameMs) {
          if (!stillOk()) return false;
          pollDuringAnim();
        }
      }
      board->setIntensity(effectiveIntensity());
      blitNameWindow(z, startCol, zCols, teeter.colBuf, tCols, nameOrigin);
      nameStaticValid = true;
      nameStaticOrigin = nameOrigin;
      // Re-settle number so Parola's zone state matches what's on screen.
      if (dual) {
        if (!paintNumberParola()) return false;
        blitNameWindow(z, startCol, zCols, teeter.colBuf, tCols, nameOrigin);
      }
      return stillOk();
    };

    auto runDualParola = [&]() -> bool {
      const textEffect_t inEff = toParolaEffect(resolved);
      uint16_t spd = parolaScrollSpeed(40);
      if (resolved == ShowMode::ANIMATION) spd = 6;
      board->displayClear(z);
      if (dual) board->displayClear(numZone);
      if (dual) board->setCharSpacing(numZone, 1);
      board->displayZoneText(z, text, PA_CENTER, spd, 0, inEff, PA_NO_EFFECT);
      board->displayReset(z);
      if (dual) {
        board->displayZoneText(numZone, numText, PA_CENTER, spd, 0, inEff, PA_NO_EFFECT);
        board->displayReset(numZone);
      }
      while (!board->getZoneStatus(z) || (dual && !board->getZoneStatus(numZone))) {
        if (!stillOk()) return false;
        board->displayAnimate();
        pollDuringAnim();
      }
      paintName(z, text);
      return stillOk();
    };

    auto finishDual = [&](int16_t nameOrigin) -> bool {
      nameStaticValid = true;
      nameStaticOrigin = nameOrigin;
      return stillOk();
    };

    auto paintNumberStatic = [&]() -> bool {
      // Match paintContent / fade — Parola center, not custom column blit.
      return paintNumberParola();
    };

    // Oversized names: transition into teeter start (offset 0), then bounce.
    if (tCols > zCols) {
      if (resolved == ShowMode::SMOTH) {
        if (!fadeNameWithNumber(0)) return false;
        startNameTeeter(z, text);
        return stillOk();
      }
      if (resolved == ShowMode::LEFT || resolved == ShowMode::RIGHT) {
        int16_t from = static_cast<int16_t>(-static_cast<int16_t>(zCols));
        int16_t step = 1;
        if (resolved == ShowMode::RIGHT) {
          from = static_cast<int16_t>(tCols);
          step = -1;
        }
        if (!scrollNameWithNumber(from, 0, step)) return false;
        if (!finishDual(0)) return false;
        startNameTeeter(z, text);
        return stillOk();
      }
      if (!scrollDisplayedOff(resolved, gen, dual ? numGen : gen, false)) return false;
      if (!runDualParola()) return false;
      startNameTeeter(z, text);
      return stillOk();
    }

    const int16_t finalOrigin = static_cast<int16_t>(
        -((static_cast<int16_t>(zCols) - static_cast<int16_t>(tCols)) / 2));

    if (resolved == ShowMode::FREEZE) {
      paintNameStatic(z, teeter.colBuf, tCols);
      if (!paintNumberStatic()) return false;
      return finishDual(finalOrigin);
    }

    if (resolved == ShowMode::SMOTH) {
      board->displayClear(z);
      if (!fadeNameWithNumber(finalOrigin)) return false;
      return stillOk();
    }

    if (resolved != ShowMode::LEFT && resolved != ShowMode::RIGHT) {
      // Exit old content with the same effect before Parola enter.
      if (!scrollDisplayedOff(resolved, gen, dual ? numGen : gen, false)) return false;
      return runDualParola();
    }

    const int16_t stepCols = 1;
    int16_t from = static_cast<int16_t>(-static_cast<int16_t>(zCols));
    int16_t step = stepCols;
    if (resolved == ShowMode::RIGHT) {
      from = static_cast<int16_t>(tCols);
      step = static_cast<int16_t>(-stepCols);
    }
    if (!scrollNameWithNumber(from, finalOrigin, step)) return false;
    return finishDual(finalOrigin);
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
      spd = 6;
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
    return runEffectAnim(z, numBuf, effect, 35, 80, gen, AnimSlot::Num);
  }

  void runCounterEnterAnim(uint8_t effect) {
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
      if (!runNameInAnim(nz, nameBuf, eff, nameGen, cz, numBuf, numGen)) return;
    } else if (cz >= 0) {
      if (!runEffectAnim(cz, numBuf, eff, 40, 0, numGen, AnimSlot::Num)) return;
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

  void runCountIncAnim(int32_t newCount, uint8_t effect) {
    board->displayShutdown(false);
    board->setIntensity(effectiveIntensity());
    paintNameOnly();
    const uint32_t gen = beginAnim(AnimSlot::Num);
    bool ok;
    if (isMilestone(newCount)) {
      ok = playFireworks(newCount, gen);
    } else {
      ok = playCountAnim(newCount, resolveEffect(effect), gen);
    }
    if (ok) restoreBlankAfterAnim();
  }

  void runCountDecAnim(int32_t newCount, uint8_t effect) {
    board->displayShutdown(false);
    board->setIntensity(effectiveIntensity());
    paintNameOnly();
    const uint32_t gen = beginAnim(AnimSlot::Num);
    if (playCountAnim(newCount, resolveEffect(effect), gen)) {
      restoreBlankAfterAnim();
    }
  }

  void drainPendingAnims() {
    while (pendingAnim.kind != PendingKind::None) {
      const PendingAnim job = pendingAnim;
      pendingAnim.kind = PendingKind::None;
      animBusy = true;
      switch (job.kind) {
        case PendingKind::CounterEnter:
          runCounterEnterAnim(job.effect);
          break;
        case PendingKind::CountInc:
          runCountIncAnim(job.count, job.effect);
          break;
        case PendingKind::CountDec:
          runCountDecAnim(job.count, job.effect);
          break;
        default:
          break;
      }
      animBusy = false;
    }
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
    if (NAME_ZONE_FLIP_UD) board->setZoneEffect(0, true, PA_FLIP_UD);
    if (NAME_ZONE_FLIP_LR) board->setZoneEffect(0, true, PA_FLIP_LR);
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
    if (scrollerTopScrolls || scrollerBottomScrolls) {
      board->displayAnimate();
      // Latch each line's first completed pass; clear it (don't reset — that
      // jumped the text back on-screen). Advance when the longest is done.
      if (scrollerTopScrolls && !scrollerTopPassLatched &&
          board->getZoneStatus(scrollerZone)) {
        scrollerTopPassLatched = true;
        board->displayClear(scrollerZone);
      }
      if (scrollerBottomScrolls && scrollerHasBottom && !scrollerBotPassLatched &&
          board->getZoneStatus(scrollerBottomZone)) {
        scrollerBotPassLatched = true;
        board->displayClear(scrollerBottomZone);
      }
      scrollerPassDone = scrollerTopPassLatched && scrollerBotPassLatched;
    } else {
      scrollerPassDone = true;
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

  pendingAnim.kind = PendingKind::CountInc;
  pendingAnim.effect = effect;
  pendingAnim.count = newCount;
  cancelInFlightAnims();
  if (animBusy) return;
  drainPendingAnims();
}

void onCountDecreased(int32_t newCount, uint8_t effect) {
  scrollerOn = false;
  if (!inited || !board || !displayVisible()) {
    applyBlankState();
    return;
  }

  pendingAnim.kind = PendingKind::CountDec;
  pendingAnim.effect = effect;
  pendingAnim.count = newCount;
  cancelInFlightAnims();
  if (animBusy) return;
  drainPendingAnims();
}

void idleShowNext(int32_t /*count*/, uint8_t effect) {
  // Soft-stop marquee flag; preferred path animates off before calling this.
  scrollerOn = false;
  scrollerPassDone = false;
  scrollerTopScrolls = false;
  scrollerBottomScrolls = false;
  if (!inited || !board || !displayVisible()) {
    applyBlankState();
    return;
  }

  pendingAnim.kind = PendingKind::CounterEnter;
  pendingAnim.effect = effect;
  pendingAnim.count = 0;
  cancelInFlightAnims();
  if (animBusy) return;
  drainPendingAnims();
}

void selectShow(uint8_t effect) {
  idleShowNext(0, effect);
}

bool scrollerActive() { return scrollerOn; }

bool scrollerCompletedPass() { return scrollerPassDone; }

// Halt marquee and leave Parola zones idle so loop() won't keep scrolling.
void haltScrollerEngine() {
  scrollerOn = false;
  scrollerPassDone = false;
  scrollerTopScrolls = false;
  scrollerBottomScrolls = false;
  scrollerHasBottom = false;
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
  if (!scrollerOn && !restore) {
    // Still allow a clear settle when idle asks to drop the panel.
  }
  haltScrollerEngine();
  if (!inited || !board) return;
  if (restore) applyBlankState();
}

bool animateDisplayedOff(uint8_t effect) {
  if (!inited || !board) return false;
  const bool fromScroller = scrollerOn;
  // Stop marquee engine; scroller exit must not reprint text.
  scrollerOn = false;
  scrollerPassDone = false;
  scrollerTopScrolls = false;
  scrollerBottomScrolls = false;
  stopNameTeeter();
  nameStaticValid = false;
  const uint32_t nameGen = beginAnim(AnimSlot::Name);
  const uint32_t numGen = beginAnim(AnimSlot::Num);
  const uint8_t resolved = resolveEffect(effect);
  if (!scrollDisplayedOff(resolved, nameGen, numGen, fromScroller)) return false;
  board->displayClear();
  return true;
}

void startManualScroller(uint8_t effectIn, const char* text, const char* bottom,
                         uint8_t effectOut) {
  if (!inited || !board) return;
  if (!displayVisible()) {
    applyBlankState();
    return;
  }

  const bool fromScroller = scrollerOn;
  // Keep current content for exit; only stop the marquee flag so loop won't fight us.
  scrollerOn = false;
  scrollerTopScrolls = false;
  scrollerBottomScrolls = false;
  stopNameTeeter();
  nameStaticValid = false;
  const uint32_t nameGen = beginAnim(AnimSlot::Name);
  const uint32_t numGen = beginAnim(AnimSlot::Num);

  const uint8_t resolvedOut = resolveEffect(effectOut);

  // Exit previous content (scroller never reprints — that flashed text after scroll-off).
  if (!scrollDisplayedOff(resolvedOut, nameGen, numGen, fromScroller)) {
    return;
  }

  const char* msg = text ? text : Counters::scrollMessage();
  if (msg && msg[0]) {
    strncpy(scrollerBuf, msg, SCROLLER_MAX_LEN);
    scrollerBuf[SCROLLER_MAX_LEN] = '\0';
  } else {
    strncpy(scrollerBuf, " ", sizeof(scrollerBuf));
  }

  const char* bot = bottom;
  if (!bot && !text) bot = Counters::scrollMessageBottom(0);
  if (bot && bot[0]) {
    strncpy(scrollerBottomBuf, bot, SCROLLER_BOTTOM_MAX_LEN);
    scrollerBottomBuf[SCROLLER_BOTTOM_MAX_LEN] = '\0';
  } else {
    scrollerBottomBuf[0] = '\0';
  }

  const uint8_t resolvedIn = resolveEffect(effectIn);
  const bool wantLeft = (resolvedIn != ShowMode::RIGHT);
  // Name zone is LR-flipped; number zone is not — use matching visual directions.
  bool nameParolaRight = !wantLeft;
  if (NAME_ZONE_FLIP_LR) nameParolaRight = !nameParolaRight;
  scrollerDir = nameParolaRight ? PA_SCROLL_RIGHT : PA_SCROLL_LEFT;
  scrollerBottomDir = wantLeft ? PA_SCROLL_LEFT : PA_SCROLL_RIGHT;

  const int8_t nz = nameZoneId();
  const int8_t cz = numberZone();
  if (nz < 0) return;

  scrollerZone = static_cast<uint8_t>(nz);
  scrollerHasBottom = (cz >= 0 && scrollerBottomBuf[0]);
  scrollerBottomZone = scrollerHasBottom ? static_cast<uint8_t>(cz) : 0;

  // Scroll only when text does not fit the zone width.
  uint8_t fitBuf[NAME_COL_BUF];
  const uint16_t topCols = buildNameColumns(scrollerBuf, fitBuf, NAME_COL_BUF);
  const uint16_t topZoneCols = zoneWidthCols(scrollerZone);
  scrollerTopScrolls = (topCols > topZoneCols);

  scrollerBottomScrolls = false;
  if (scrollerHasBottom) {
    uint8_t botFit[64];
    const uint16_t botCols = buildNameColumns(scrollerBottomBuf, botFit, sizeof(botFit));
    const uint16_t botZoneCols = zoneWidthCols(scrollerBottomZone);
    scrollerBottomScrolls = (botCols > botZoneCols);
  }

  scrollerOn = true;
  scrollerTopPassLatched = !scrollerTopScrolls;  // static = already "done"
  scrollerBotPassLatched = !scrollerBottomScrolls;
  scrollerPassDone = scrollerTopPassLatched && scrollerBotPassLatched;

  board->displayShutdown(false);
  board->setIntensity(effectiveIntensity());
  board->displayClear();

  const uint16_t spd = parolaScrollSpeed(40);
  board->setCharSpacing(scrollerZone, 1);
  if (scrollerTopScrolls) {
    board->displayZoneText(scrollerZone, scrollerBuf, PA_LEFT, spd, 0, scrollerDir, scrollerDir);
  } else {
    board->displayZoneText(scrollerZone, scrollerBuf, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  }
  board->displayReset(scrollerZone);

  if (cz >= 0) {
    board->displayClear(cz);
    if (scrollerHasBottom) {
      board->setCharSpacing(scrollerBottomZone, 1);
      if (scrollerBottomScrolls) {
        board->displayZoneText(scrollerBottomZone, scrollerBottomBuf, PA_LEFT, spd, 0,
                               scrollerBottomDir, scrollerBottomDir);
      } else {
        board->displayZoneText(scrollerBottomZone, scrollerBottomBuf, PA_CENTER, 0, 0,
                               PA_PRINT, PA_NO_EFFECT);
      }
      board->displayReset(scrollerBottomZone);
    }
  }
}

}  // namespace Display
