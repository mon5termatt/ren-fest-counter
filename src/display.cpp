#include "display.h"
#include "counters.h"
#include "show_mode.h"
#include "wiz_remote.h"
#include "remote_map.h"
#include "idle_cycle.h"
#include "wifi_mgr.h"
#include "wifi_config.h"
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <cstring>
#include <cstdio>

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
    uint8_t speed = 0;  // 0 = use per-anim default
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

  uint8_t percentToIntensity(uint8_t level) {
    // blankBrightness is already MAX7219 0–15 (0 = off handled elsewhere).
    return level > 15 ? 15 : level;
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

  // teeterSpeed 1..10 unchanged; 20 = 2× old max (20ms → 10ms)
  uint16_t teeterFrameMs() {
    uint8_t s = Counters::teeterSpeed();
    if (s < 1) s = 1;
    if (s > 20) s = 20;
    if (s <= 10) {
      return static_cast<uint16_t>(90 - (s * 7));
    }
    // 10 → 20ms, 20 → 10ms
    return static_cast<uint16_t>(20 - (s - 10));
  }

  // Active transition speed (1–20) for the effect currently playing.
  uint8_t currentAnimSpeed = 5;

  void useAnimSpeed(uint8_t speed1to20) {
    uint8_t s = speed1to20;
    if (s < 1) s = 1;
    if (s > 20) s = 20;
    currentAnimSpeed = s;
  }

  // overrideSpd 0 → per-anim default from Counters::effectSpeed
  void useEffectSpeed(uint8_t resolvedMode, uint8_t overrideSpd = 0) {
    useAnimSpeed(overrideSpd ? overrideSpd : Counters::effectSpeed(resolvedMode));
  }

  void useMarqueeSpeed() {
    useAnimSpeed(Counters::scrollSpeed());
  }

  // Transition scroll: ms between column steps (lower = faster overall).
  // 1–10 match prior curve; 20 = 2× old max (8ms → 4ms).
  uint16_t transitionFrameMs() {
    uint8_t s = currentAnimSpeed;
    if (s < 1) s = 1;
    if (s > 20) s = 20;
    if (s <= 10) {
      return static_cast<uint16_t>(28 - ((s - 1) * 20) / 9);
    }
    return static_cast<uint16_t>(8 - ((s - 10) * 4) / 10);
  }

  // Horizontal scroll progress per frame: 1 at speed 1 → 3 at 10 → 6 at 20 (2× old max).
  uint8_t transitionStride() {
    uint8_t s = currentAnimSpeed;
    if (s < 1) s = 1;
    if (s > 20) s = 20;
    if (s <= 10) {
      return static_cast<uint8_t>(1 + ((s - 1) * 2) / 9);
    }
    return static_cast<uint8_t>(3 + ((s - 10) * 3) / 10);
  }

  // Parola effect speed (lower = faster); scale with currentAnimSpeed.
  // 1–10: prior (base*6)/s. 11–20: same formula → s=20 is half the tick of s=10 (2×).
  uint16_t parolaScrollSpeed(uint16_t base) {
    uint8_t s = currentAnimSpeed;
    if (s < 1) s = 1;
    if (s > 20) s = 20;
    const int scaled = (static_cast<int>(base) * 6) / static_cast<int>(s);
    // min 3 so s=20 can reach half of s=10 (old floor 6 capped the top)
    if (scaled < 3) return 3;
    if (scaled > 80) return 80;
    return static_cast<uint16_t>(scaled);
  }

  // Laser wipe/slice run in a busy-wait that also polls IR/WiFi. One
  // displayAnimate() per poll can't beat that overhead — catch up to the
  // target tick for speeds above 10 only (1–10 keep stock Parola timing).
  // Set to a ShowMode while waiting on that effect (ANIMATION needs huge bursts).
  uint8_t parolaPumpMode = 0xFF;

  struct {
    uint32_t lastMs = 0;
    int16_t accum = 0;
  } parolaPump;

  void resetParolaPump() {
    parolaPump.lastMs = millis();
    parolaPump.accum = 0;
  }

  void beginParolaPump(uint8_t mode) {
    parolaPumpMode = mode;
    resetParolaPump();
  }

  void endParolaPump() { parolaPumpMode = 0xFF; }

  void pumpParolaAnimate() {
    uint8_t s = currentAnimSpeed;
    if (s < 1) s = 1;
    if (s > 20) s = 20;

    // PA_RANDOM reveals 1 pixel per animate (~88+ frames). Burst hard with speed.
    if (parolaPumpMode == ShowMode::ANIMATION) {
      // s=1 → 1, s=10 → 7, s=20 → 26 frames per poll
      const uint8_t burst =
          static_cast<uint8_t>(1 + (static_cast<uint16_t>(s) * s) / 16);
      board->setSpeed(0);
      for (uint8_t i = 0; i < burst; i++) {
        board->displayAnimate();
      }
      return;
    }

    if (s <= 10) {
      board->displayAnimate();
      return;
    }

    uint16_t period = parolaScrollSpeed(40);  // ~22..12 ms for s=11..20
    if (period < 1) period = 1;

    const uint32_t now = millis();
    if (parolaPump.lastMs == 0) {
      parolaPump.lastMs = now;
      return;
    }
    uint32_t dt = now - parolaPump.lastMs;
    parolaPump.lastMs = now;
    if (dt > 50) dt = 50;
    parolaPump.accum =
        static_cast<int16_t>(parolaPump.accum + static_cast<int16_t>(dt));

    if (parolaPump.accum < static_cast<int16_t>(period)) return;

    board->setSpeed(0);
    uint8_t n = 0;
    while (parolaPump.accum >= static_cast<int16_t>(period) && n < 4) {
      parolaPump.accum =
          static_cast<int16_t>(parolaPump.accum - static_cast<int16_t>(period));
      board->displayAnimate();
      n++;
    }
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

  // Bottom-zone teeter for status lines (SSID / etc.) — number strip, no flips.
  struct BotTeeter {
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
  } botTeeter;

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
  uint16_t scrollerTopSpd = 6;
  uint16_t scrollerBotSpd = 6;
  // Night / web Start: keep rotating through all scrollMessages until stopped.
  bool scrollerCycleAll = false;
  bool scrollerHolding = false;       // static message hold between advances
  bool scrollerAdvancing = false;     // re-entrancy guard
  uint8_t scrollerCycleIdx = 0;
  uint8_t scrollerCycleInEff = ShowMode::LEFT;
  uint8_t scrollerCycleOutEff = ShowMode::LEFT;
  uint8_t scrollerCycleSpeed = 0;
  uint32_t scrollerHoldStartMs = 0;

  // Setup-menu LED test patterns (full daisy-chain).
  enum class TestPattern : uint8_t { Off = 0, AllOn, Checker };
  TestPattern testPattern = TestPattern::Off;
  uint8_t checkerPhase = 0;
  uint32_t checkerNextMs = 0;
  constexpr uint32_t CHECKER_PERIOD_MS = 280;

  void clearTestPattern() { testPattern = TestPattern::Off; }

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
  void stopBotTeeter() { botTeeter.active = false; }

  void restoreNameBlit();  // after blit helpers
  void blitBotTeeterFrame();
  void tickBotTeeter();
  void paintBotLine(uint8_t z, const char* text);

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

  // Full text width in columns (no buffer cap) — matches Parola getTextWidth
  // with charSpacing=1 (spacing only after glyphs that drew).
  uint16_t measureTextColumns(const char* text) {
    MD_MAX72XX* mx = board->getGraphicObject();
    if (!mx || !text) return 0;
    uint16_t sum = 0;
    for (const char* p = text; *p; p++) {
      uint8_t glyph[10];
      uint8_t w = mx->getChar(static_cast<uint8_t>(*p), sizeof(glyph), glyph);
      if (w > sizeof(glyph)) w = sizeof(glyph);
      sum = static_cast<uint16_t>(sum + w);
      if (w != 0 && *(p + 1)) sum = static_cast<uint16_t>(sum + 1);
    }
    return sum;
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

  // Vertical wipe-cursor (matches PA_WIPE_CURSOR / Laser Left): a full-row light
  // bar sweeps and reveals text behind it. Parola SCAN_VERT only peels rows.
  bool laserVertWipeZones(int8_t nz, const char* top, int8_t cz, const char* bot,
                          bool up, bool entering,
                          uint32_t nameGen, uint32_t numGen) {
    MD_MAX72XX* mx = board->getGraphicObject();
    if (!mx) return false;

    auto stillOk = [&]() -> bool {
      if (nz >= 0 && !animCurrent(AnimSlot::Name, nameGen)) return false;
      if (cz >= 0 && !animCurrent(AnimSlot::Num, numGen)) return false;
      return true;
    };

    auto settlePrint = [&](int8_t z, const char* text) -> bool {
      if (z < 0 || !text) return true;
      board->setCharSpacing(static_cast<uint8_t>(z), 1);
      board->displayClear(static_cast<uint8_t>(z));
      board->displayZoneText(static_cast<uint8_t>(z), text, PA_CENTER, 0, 0, PA_PRINT,
                             PA_NO_EFFECT);
      board->displayReset(static_cast<uint8_t>(z));
      while (!board->getZoneStatus(static_cast<uint8_t>(z))) {
        if (!stillOk()) return false;
        board->displayAnimate();
        pollDuringAnim();
      }
      return stillOk();
    };

    // Exit: snapshot what's already on screen (don't reprint — that flashes).
    if (entering) {
      if (!settlePrint(nz, top)) return false;
      if (!settlePrint(cz, bot)) return false;
    }

    uint16_t nStart = 0, nEnd = 0, cStart = 0, cEnd = 0;
    uint16_t nCols = 0, cCols = 0;
    if (nz >= 0) {
      board->getDisplayExtent(static_cast<uint8_t>(nz), nStart, nEnd);
      nCols = static_cast<uint16_t>(nEnd - nStart + 1);
    }
    if (cz >= 0) {
      board->getDisplayExtent(static_cast<uint8_t>(cz), cStart, cEnd);
      cCols = static_cast<uint16_t>(cEnd - cStart + 1);
    }

    constexpr uint16_t kMaxCols = 96;
    uint8_t nSnap[kMaxCols];
    uint8_t cSnap[kMaxCols];
    if (nCols > kMaxCols) nCols = kMaxCols;
    if (cCols > kMaxCols) cCols = kMaxCols;
    for (uint16_t i = 0; i < nCols; i++) nSnap[i] = mx->getColumn(nStart + i);
    for (uint16_t i = 0; i < cCols; i++) cSnap[i] = mx->getColumn(cStart + i);

    // Same tick as Laser Left (parolaScrollSpeed). Catch up after pollDuringAnim
    // so high speeds aren't flattened to one row per IR/WiFi poll.
    uint16_t period = parolaScrollSpeed(40);
    if (period < 1) period = 1;

    // Name strip is FLIP_UD — invert sweep so "Up" moves toward visual top.
    const bool sweepUp = NAME_ZONE_FLIP_UD ? !up : up;

    auto paintStep = [&](int step) {
      const int barBit = sweepUp ? step : (7 - step);
      uint8_t revealMask;
      if (entering) {
        revealMask = sweepUp
                         ? static_cast<uint8_t>((barBit == 0) ? 0 : ((1u << barBit) - 1u))
                         : static_cast<uint8_t>(0xFFu << (barBit + 1));
      } else {
        revealMask = sweepUp
                         ? static_cast<uint8_t>(0xFFu << (barBit + 1))
                         : static_cast<uint8_t>((1u << barBit) - 1u);
      }
      const uint8_t barMask = static_cast<uint8_t>(1u << barBit);

      auto paint = [&](uint16_t start, uint16_t cols, const uint8_t* snap) {
        for (uint16_t i = 0; i < cols; i++) {
          // Light-bar only on columns with glyph ink (skip empty padding).
          uint8_t out = static_cast<uint8_t>(snap[i] & revealMask);
          if (snap[i]) out = static_cast<uint8_t>(out | barMask);
          mx->setColumn(start + i, out);
        }
      };
      paint(nStart, nCols, nSnap);
      paint(cStart, cCols, cSnap);
      mx->update();
    };

    uint32_t lastMs = millis();
    int16_t accum = 0;
    int step = 0;
    while (step < 8) {
      if (!stillOk()) return false;
      pollDuringAnim();
      const uint32_t now = millis();
      uint32_t dt = now - lastMs;
      lastMs = now;
      if (dt > 50) dt = 50;
      accum = static_cast<int16_t>(accum + static_cast<int16_t>(dt));
      if (accum < static_cast<int16_t>(period)) continue;
      while (accum >= static_cast<int16_t>(period) && step < 8) {
        accum = static_cast<int16_t>(accum - static_cast<int16_t>(period));
        paintStep(step);
        step++;
      }
    }

    if (entering) {
      for (uint16_t i = 0; i < nCols; i++) mx->setColumn(nStart + i, nSnap[i]);
      for (uint16_t i = 0; i < cCols; i++) mx->setColumn(cStart + i, cSnap[i]);
      mx->update();
    } else {
      board->displayClear();
    }
    return stillOk();
  }

  // Sand-pile: each glyph row falls from the top and stacks at the bottom.
  bool pilingSandZones(int8_t nz, const char* top, int8_t cz, const char* bot,
                       bool entering, uint32_t nameGen, uint32_t numGen) {
    MD_MAX72XX* mx = board->getGraphicObject();
    if (!mx) return false;

    auto stillOk = [&]() -> bool {
      if (nz >= 0 && !animCurrent(AnimSlot::Name, nameGen)) return false;
      if (cz >= 0 && !animCurrent(AnimSlot::Num, numGen)) return false;
      return true;
    };

    auto settlePrint = [&](int8_t z, const char* text) -> bool {
      if (z < 0 || !text) return true;
      board->setCharSpacing(static_cast<uint8_t>(z), 1);
      board->displayClear(static_cast<uint8_t>(z));
      board->displayZoneText(static_cast<uint8_t>(z), text, PA_CENTER, 0, 0, PA_PRINT,
                             PA_NO_EFFECT);
      board->displayReset(static_cast<uint8_t>(z));
      while (!board->getZoneStatus(static_cast<uint8_t>(z))) {
        if (!stillOk()) return false;
        board->displayAnimate();
        pollDuringAnim();
      }
      return stillOk();
    };

    if (entering) {
      if (!settlePrint(nz, top)) return false;
      if (!settlePrint(cz, bot)) return false;
    }

    uint16_t nStart = 0, nEnd = 0, cStart = 0, cEnd = 0;
    uint16_t nCols = 0, cCols = 0;
    if (nz >= 0) {
      board->getDisplayExtent(static_cast<uint8_t>(nz), nStart, nEnd);
      nCols = static_cast<uint16_t>(nEnd - nStart + 1);
    }
    if (cz >= 0) {
      board->getDisplayExtent(static_cast<uint8_t>(cz), cStart, cEnd);
      cCols = static_cast<uint16_t>(cEnd - cStart + 1);
    }

    constexpr uint16_t kMaxCols = 96;
    uint8_t nSnap[kMaxCols];
    uint8_t cSnap[kMaxCols];
    if (nCols > kMaxCols) nCols = kMaxCols;
    if (cCols > kMaxCols) cCols = kMaxCols;
    for (uint16_t i = 0; i < nCols; i++) nSnap[i] = mx->getColumn(nStart + i);
    for (uint16_t i = 0; i < cCols; i++) cSnap[i] = mx->getColumn(cStart + i);

    // Clear before piling in (start empty, sand falls in).
    if (entering) {
      for (uint16_t i = 0; i < nCols; i++) mx->setColumn(nStart + i, 0);
      for (uint16_t i = 0; i < cCols; i++) mx->setColumn(cStart + i, 0);
      mx->update();
    }

    // Bind to the Piling slider directly. Period alone can't go faster than one
    // SPI update per row — high speeds must skip rows (stride), like Animation.
    useAnimSpeed(Counters::effectSpeed(ShowMode::PILING));
    uint8_t s = currentAnimSpeed;
    if (s < 1) s = 1;
    if (s > 20) s = 20;
    // s=1 → 1 row/tick (~full 8-step falls); s=20 → 4 rows/tick (~2× prior max).
    const uint8_t stride = static_cast<uint8_t>(1 + (s - 1) / 5);
    // Inter-tick pause: s=1 → 28ms, s=20 → 2ms (then catch-up after polls).
    uint16_t period = static_cast<uint16_t>(30 - s);
    if (period < 2) period = 2;

    // Per-zone device bits: only the name strip uses FLIP_UD.
    struct ZoneOri {
      int bottomBit;
      int topBit;
      int fallDir;  // top → bottom
      int pileDir;  // bottom → top (stack order)
    };
    auto oriFor = [](bool flipUD) -> ZoneOri {
      return ZoneOri{
          flipUD ? 7 : 0,
          flipUD ? 0 : 7,
          flipUD ? 1 : -1,
          flipUD ? -1 : 1,
      };
    };
    // Out uses steady-state orientation; in is reversed (matches visual hardware).
    const ZoneOri nOri = oriFor(entering ? !NAME_ZONE_FLIP_UD : NAME_ZONE_FLIP_UD);
    const ZoneOri cOri = oriFor(entering);  // number strip: flip only for pile-in

    auto layerHasInk = [&](const uint8_t* snap, uint16_t cols, int targetBit) -> bool {
      const uint8_t m = static_cast<uint8_t>(1u << targetBit);
      for (uint16_t i = 0; i < cols; i++) {
        if (snap[i] & m) return true;
      }
      return false;
    };

    auto paintZone = [&](uint16_t start, uint16_t cols, const uint8_t* snap,
                         uint8_t settledMask, int fallBit, int targetBit, bool showFall) {
      const uint8_t fallMask = static_cast<uint8_t>(1u << fallBit);
      const uint8_t tgtMask = static_cast<uint8_t>(1u << targetBit);
      for (uint16_t i = 0; i < cols; i++) {
        uint8_t out = static_cast<uint8_t>(snap[i] & settledMask);
        if (showFall && (snap[i] & tgtMask)) {
          out = static_cast<uint8_t>(out | fallMask);
        }
        mx->setColumn(start + i, out);
      }
    };

    auto paintFrame = [&](uint8_t nSettled, int nFall, int nTarget, uint8_t cSettled,
                          int cFall, int cTarget, bool showFall) {
      paintZone(nStart, nCols, nSnap, nSettled, nFall, nTarget, showFall);
      paintZone(cStart, cCols, cSnap, cSettled, cFall, cTarget, showFall);
      mx->update();
    };

    uint32_t lastMs = millis();
    int16_t accum = 0;
    auto waitTick = [&]() -> bool {
      while (true) {
        if (!stillOk()) return false;
        pollDuringAnim();
        const uint32_t now = millis();
        uint32_t dt = now - lastMs;
        lastMs = now;
        if (dt > 50) dt = 50;
        accum = static_cast<int16_t>(accum + static_cast<int16_t>(dt));
        if (accum < static_cast<int16_t>(period)) continue;
        accum = static_cast<int16_t>(accum - static_cast<int16_t>(period));
        return true;
      }
    };

    auto nextY = [&](int y, int yEnd) -> int {
      if (y >= yEnd) return yEnd + 1;
      const int n = y + static_cast<int>(stride);
      if (n >= yEnd) return yEnd;  // always land on the target row
      return n;
    };

    if (entering) {
      uint8_t nSettled = 0;
      uint8_t cSettled = 0;
      for (int layer = 0; layer < 8; layer++) {
        const int nTarget = nOri.bottomBit + nOri.pileDir * layer;
        const int cTarget = cOri.bottomBit + cOri.pileDir * layer;
        const bool ink = layerHasInk(nSnap, nCols, nTarget) ||
                         layerHasInk(cSnap, cCols, cTarget);
        if (!ink) {
          nSettled = static_cast<uint8_t>(nSettled | (1u << nTarget));
          cSettled = static_cast<uint8_t>(cSettled | (1u << cTarget));
          continue;
        }
        const int yEnd = (nTarget > nOri.topBit) ? (nTarget - nOri.topBit)
                                                 : (nOri.topBit - nTarget);
        for (int y = 0; y <= yEnd; y = nextY(y, yEnd)) {
          if (!waitTick()) return false;
          const int nFall = nOri.topBit + nOri.fallDir * y;
          const int cFall = cOri.topBit + cOri.fallDir * y;
          paintFrame(nSettled, nFall, nTarget, cSettled, cFall, cTarget, true);
          if (y == yEnd) break;
        }
        nSettled = static_cast<uint8_t>(nSettled | (1u << nTarget));
        cSettled = static_cast<uint8_t>(cSettled | (1u << cTarget));
      }
      for (uint16_t i = 0; i < nCols; i++) mx->setColumn(nStart + i, nSnap[i]);
      for (uint16_t i = 0; i < cCols; i++) mx->setColumn(cStart + i, cSnap[i]);
      mx->update();
    } else {
      uint8_t nSettled = 0xFF;
      uint8_t cSettled = 0xFF;
      for (int layer = 7; layer >= 0; layer--) {
        const int nTarget = nOri.bottomBit + nOri.pileDir * layer;
        const int cTarget = cOri.bottomBit + cOri.pileDir * layer;
        const bool ink = layerHasInk(nSnap, nCols, nTarget) ||
                         layerHasInk(cSnap, cCols, cTarget);
        if (!ink) {
          nSettled = static_cast<uint8_t>(nSettled & ~(1u << nTarget));
          cSettled = static_cast<uint8_t>(cSettled & ~(1u << cTarget));
          continue;
        }
        nSettled = static_cast<uint8_t>(nSettled & ~(1u << nTarget));
        cSettled = static_cast<uint8_t>(cSettled & ~(1u << cTarget));
        const int yEnd = (nTarget > nOri.topBit) ? (nTarget - nOri.topBit)
                                                 : (nOri.topBit - nTarget);
        for (int y = 0; y <= yEnd; y = nextY(y, yEnd)) {
          if (!waitTick()) return false;
          const int nFall = nTarget - nOri.fallDir * y;
          const int cFall = cTarget - cOri.fallDir * y;
          paintFrame(nSettled, nFall, nTarget, cSettled, cFall, cTarget, true);
          if (y == yEnd) break;
        }
        if (!waitTick()) return false;
        paintFrame(nSettled, nOri.topBit, nTarget, cSettled, cOri.topBit, cTarget,
                   false);
      }
      board->displayClear();
    }
    return stillOk();
  }

  // True random pixel dissolve across the full zone (Parola PA_RANDOM repeats
  // an 11-column mask, so it looks striped rather than random).
  bool animationRandomZones(int8_t nz, const char* top, int8_t cz, const char* bot,
                            bool entering, uint32_t nameGen, uint32_t numGen) {
    MD_MAX72XX* mx = board->getGraphicObject();
    if (!mx) return false;

    auto stillOk = [&]() -> bool {
      if (nz >= 0 && !animCurrent(AnimSlot::Name, nameGen)) return false;
      if (cz >= 0 && !animCurrent(AnimSlot::Num, numGen)) return false;
      return true;
    };

    auto settlePrint = [&](int8_t z, const char* text) -> bool {
      if (z < 0 || !text) return true;
      board->setCharSpacing(static_cast<uint8_t>(z), 1);
      board->displayClear(static_cast<uint8_t>(z));
      board->displayZoneText(static_cast<uint8_t>(z), text, PA_CENTER, 0, 0, PA_PRINT,
                             PA_NO_EFFECT);
      board->displayReset(static_cast<uint8_t>(z));
      while (!board->getZoneStatus(static_cast<uint8_t>(z))) {
        if (!stillOk()) return false;
        board->displayAnimate();
        pollDuringAnim();
      }
      return stillOk();
    };

    if (entering) {
      if (!settlePrint(nz, top)) return false;
      if (!settlePrint(cz, bot)) return false;
    }

    uint16_t nStart = 0, nEnd = 0, cStart = 0, cEnd = 0;
    uint16_t nCols = 0, cCols = 0;
    if (nz >= 0) {
      board->getDisplayExtent(static_cast<uint8_t>(nz), nStart, nEnd);
      nCols = static_cast<uint16_t>(nEnd - nStart + 1);
    }
    if (cz >= 0) {
      board->getDisplayExtent(static_cast<uint8_t>(cz), cStart, cEnd);
      cCols = static_cast<uint16_t>(cEnd - cStart + 1);
    }

    constexpr uint16_t kMaxCols = 96;
    uint8_t nSnap[kMaxCols];
    uint8_t cSnap[kMaxCols];
    if (nCols > kMaxCols) nCols = kMaxCols;
    if (cCols > kMaxCols) cCols = kMaxCols;
    for (uint16_t i = 0; i < nCols; i++) nSnap[i] = mx->getColumn(nStart + i);
    for (uint16_t i = 0; i < cCols; i++) cSnap[i] = mx->getColumn(cStart + i);

    // Pack lit pixels: high byte = zone (0=name,1=num), low = col index; bit in separate nibble via packed uint16
    // layout: (absCol << 3) | bit  — absCol fits in 13 bits for <96*2
    constexpr uint16_t kMaxPix = 768;
    uint16_t pix[kMaxPix];
    uint16_t nPix = 0;
    auto addCol = [&](uint16_t absCol, uint8_t snapByte) {
      for (uint8_t b = 0; b < 8 && nPix < kMaxPix; b++) {
        if (snapByte & (1u << b)) {
          pix[nPix++] = static_cast<uint16_t>((absCol << 3) | b);
        }
      }
    };
    for (uint16_t i = 0; i < nCols; i++) addCol(static_cast<uint16_t>(nStart + i), nSnap[i]);
    for (uint16_t i = 0; i < cCols; i++) addCol(static_cast<uint16_t>(cStart + i), cSnap[i]);

    // Fisher–Yates shuffle
    for (uint16_t i = nPix; i > 1; i--) {
      const uint16_t j = static_cast<uint16_t>(random(i));
      const uint16_t tmp = pix[i - 1];
      pix[i - 1] = pix[j];
      pix[j] = tmp;
    }

    if (entering) {
      for (uint16_t i = 0; i < nCols; i++) mx->setColumn(nStart + i, 0);
      for (uint16_t i = 0; i < cCols; i++) mx->setColumn(cStart + i, 0);
      mx->update();
    }

    uint8_t s = currentAnimSpeed;
    if (s < 1) s = 1;
    if (s > 20) s = 20;
    // Same tick as Laser / Piling. Burst sized so total steps stay visible:
    // s=1 → ~90 steps (~7s), s=10 → ~60 (~1.4s), s=20 → ~30 (~0.4s).
    uint16_t period = parolaScrollSpeed(40);
    if (period < 1) period = 1;
    const uint16_t steps =
        static_cast<uint16_t>(30 + (20 - static_cast<uint16_t>(s)) * 3);
    uint16_t burst = 1;
    if (nPix > 0) {
      burst = static_cast<uint16_t>((nPix + steps - 1) / steps);
      if (burst < 1) burst = 1;
    }

    uint32_t lastMs = millis();
    int16_t accum = 0;
    auto waitTick = [&]() -> bool {
      while (true) {
        if (!stillOk()) return false;
        pollDuringAnim();
        const uint32_t now = millis();
        uint32_t dt = now - lastMs;
        lastMs = now;
        if (dt > 50) dt = 50;
        accum = static_cast<int16_t>(accum + static_cast<int16_t>(dt));
        if (accum < static_cast<int16_t>(period)) continue;
        accum = static_cast<int16_t>(accum - static_cast<int16_t>(period));
        return true;
      }
    };

    uint16_t done = 0;
    while (done < nPix) {
      if (!waitTick()) return false;
      const uint16_t remain = static_cast<uint16_t>(nPix - done);
      const uint16_t n = (remain < burst) ? remain : burst;
      for (uint16_t k = 0; k < n; k++) {
        const uint16_t p = pix[done++];
        const uint16_t absCol = static_cast<uint16_t>(p >> 3);
        const uint8_t bit = static_cast<uint8_t>(p & 7u);
        const uint8_t mask = static_cast<uint8_t>(1u << bit);
        uint8_t col = mx->getColumn(absCol);
        if (entering) {
          // Restore this pixel from the snapshot
          uint8_t src = 0;
          if (nz >= 0 && absCol >= nStart && absCol < nStart + nCols) {
            src = nSnap[absCol - nStart];
          } else if (cz >= 0 && absCol >= cStart && absCol < cStart + cCols) {
            src = cSnap[absCol - cStart];
          }
          col = static_cast<uint8_t>(col | (src & mask));
        } else {
          col = static_cast<uint8_t>(col & ~mask);
        }
        mx->setColumn(absCol, col);
      }
      mx->update();
    }

    if (entering) {
      for (uint16_t i = 0; i < nCols; i++) mx->setColumn(nStart + i, nSnap[i]);
      for (uint16_t i = 0; i < cCols; i++) mx->setColumn(cStart + i, cSnap[i]);
      mx->update();
    } else {
      board->displayClear();
    }
    return stillOk();
  }

  // Exit with the same Parola effect used for enter (toParolaEffect).
  // resolved = already resolveEffect()'d.
  bool scrollDisplayedOff(uint8_t resolved, uint32_t nameGen, uint32_t numGen,
                          bool fromScroller) {
    // Caller sets currentAnimSpeed via useEffectSpeed / useAnimSpeed.
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

    if (ShowMode::isLaserVert(resolved)) {
      return laserVertWipeZones(nz, top, cz, bot,
                                resolved == ShowMode::LASER_UP, false, nameGen, numGen);
    }
    if (resolved == ShowMode::PILING) {
      return pilingSandZones(nz, top, cz, bot, false, nameGen, numGen);
    }
    if (resolved == ShowMode::ANIMATION) {
      return animationRandomZones(nz, top, cz, bot, false, nameGen, numGen);
    }

    const textEffect_t outEff = toParolaEffect(resolved);
    uint16_t spd = parolaScrollSpeed(40);
    if (resolved == ShowMode::ANIMATION) spd = 0;  // pump bursts pixels; tick unused

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

    beginParolaPump(resolved);
    while ((nz >= 0 && !board->getZoneStatus(static_cast<uint8_t>(nz))) ||
           (cz >= 0 && !board->getZoneStatus(static_cast<uint8_t>(cz)))) {
      if (!stillOk()) {
        endParolaPump();
        return false;
      }
      pumpParolaAnimate();
      pollDuringAnim();
    }
    endParolaPump();
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
    if (resolved == ShowMode::ANIMATION) spd = 0;  // pump bursts pixels; tick unused
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
      if (ShowMode::isLaserVert(resolved)) {
        if (!laserVertWipeZones(static_cast<int8_t>(z), text,
                                dual ? static_cast<int8_t>(numZone) : static_cast<int8_t>(-1),
                                dual ? numText : nullptr,
                                resolved == ShowMode::LASER_UP, true, gen,
                                dual ? numGen : gen)) {
          return false;
        }
        paintName(z, text);
        return stillOk();
      }
      if (resolved == ShowMode::PILING) {
        if (!pilingSandZones(static_cast<int8_t>(z), text,
                             dual ? static_cast<int8_t>(numZone) : static_cast<int8_t>(-1),
                             dual ? numText : nullptr, true, gen,
                             dual ? numGen : gen)) {
          return false;
        }
        paintName(z, text);
        return stillOk();
      }
      if (resolved == ShowMode::ANIMATION) {
        if (!animationRandomZones(static_cast<int8_t>(z), text,
                                  dual ? static_cast<int8_t>(numZone) : static_cast<int8_t>(-1),
                                  dual ? numText : nullptr, true, gen,
                                  dual ? numGen : gen)) {
          return false;
        }
        paintName(z, text);
        return stillOk();
      }
      const textEffect_t inEff = toParolaEffect(resolved);
      uint16_t spd = parolaScrollSpeed(40);
      if (resolved == ShowMode::ANIMATION) spd = 0;  // pump bursts pixels; tick unused
      board->displayClear(z);
      if (dual) board->displayClear(numZone);
      if (dual) board->setCharSpacing(numZone, 1);
      board->displayZoneText(z, text, PA_CENTER, spd, 0, inEff, PA_NO_EFFECT);
      board->displayReset(z);
      if (dual) {
        board->displayZoneText(numZone, numText, PA_CENTER, spd, 0, inEff, PA_NO_EFFECT);
        board->displayReset(numZone);
      }
      beginParolaPump(resolved);
      while (!board->getZoneStatus(z) || (dual && !board->getZoneStatus(numZone))) {
        if (!stillOk()) {
          endParolaPump();
          return false;
        }
        pumpParolaAnimate();
        pollDuringAnim();
      }
      endParolaPump();
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

  void blitBotTeeterFrame() {
    if (!botTeeter.active) return;
    blitZoneColumns(botTeeter.zoneStart, botTeeter.zoneCols, botTeeter.colBuf,
                    botTeeter.textCols, botTeeter.offset, false, false, true);
  }

  void tickBotTeeter() {
    if (!botTeeter.active || botTeeter.overflow == 0) return;
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - botTeeter.nextMs) < 0) return;

    if (botTeeter.pausing) {
      botTeeter.pausing = false;
      botTeeter.nextMs = now + teeterFrameMs();
      return;
    }

    botTeeter.offset = static_cast<int16_t>(botTeeter.offset + botTeeter.dir);
    if (botTeeter.offset >= static_cast<int16_t>(botTeeter.overflow)) {
      botTeeter.offset = static_cast<int16_t>(botTeeter.overflow);
      botTeeter.dir = -1;
      botTeeter.pausing = true;
      botTeeter.nextMs = now + TEETER_END_PAUSE_MS;
    } else if (botTeeter.offset <= 0) {
      botTeeter.offset = 0;
      botTeeter.dir = 1;
      botTeeter.pausing = true;
      botTeeter.nextMs = now + TEETER_END_PAUSE_MS;
    } else {
      botTeeter.nextMs = now + teeterFrameMs();
    }
    blitBotTeeterFrame();
  }

  void paintBotStatic(uint8_t z, const uint8_t* colBuf, uint16_t textCols) {
    uint16_t startCol = 0, endCol = 0;
    board->getDisplayExtent(z, startCol, endCol);
    const uint16_t zCols = zoneWidthCols(z);
    const int16_t origin =
        (textCols >= zCols) ? 0
                            : static_cast<int16_t>(-((static_cast<int16_t>(zCols) -
                                                      static_cast<int16_t>(textCols)) /
                                                     2));
    botTeeter.zone = z;
    botTeeter.zoneStart = startCol;
    botTeeter.zoneCols = zCols;
    botTeeter.textCols = textCols;
    if (colBuf != botTeeter.colBuf && textCols > 0) {
      memcpy(botTeeter.colBuf, colBuf, textCols);
    }
    blitZoneColumns(startCol, zCols, botTeeter.colBuf, textCols, origin, false, false,
                    true);
  }

  void startBotTeeter(uint8_t z, const char* text) {
    uint16_t startCol = 0, endCol = 0;
    board->getDisplayExtent(z, startCol, endCol);
    const uint16_t zCols = zoneWidthCols(z);
    const uint16_t tCols = buildNameColumns(text, botTeeter.colBuf, NAME_COL_BUF);
    botTeeter.active = true;
    botTeeter.zone = z;
    botTeeter.zoneStart = startCol;
    botTeeter.zoneCols = zCols;
    botTeeter.textCols = tCols;
    botTeeter.overflow = (tCols > zCols) ? static_cast<uint16_t>(tCols - zCols) : 0;
    botTeeter.offset = 0;
    botTeeter.dir = 1;
    botTeeter.pausing = true;
    botTeeter.nextMs = millis() + TEETER_END_PAUSE_MS;
    blitBotTeeterFrame();
  }

  void paintBotLine(uint8_t z, const char* text) {
    const uint16_t tCols = buildNameColumns(text, botTeeter.colBuf, NAME_COL_BUF);
    const uint16_t zCols = zoneWidthCols(z);
    if (tCols <= zCols) {
      stopBotTeeter();
      paintBotStatic(z, botTeeter.colBuf, tCols);
    } else {
      board->displayClear(z);
      startBotTeeter(z, text);
    }
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
    stopBotTeeter();
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
    clearTestPattern();

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
                   uint16_t speed, uint16_t pauseMs, uint32_t gen, AnimSlot slot,
                   uint8_t pumpMode = 0xFF) {
    if (!animCurrent(slot, gen)) return false;

    board->displayClear(zone);
    board->displayZoneText(zone, text, PA_CENTER, speed, pauseMs, inEff, outEff);
    board->displayReset(zone);
    beginParolaPump(pumpMode);
    while (!board->getZoneStatus(zone)) {
      if (!animCurrent(slot, gen)) {
        endParolaPump();
        return false;
      }
      pumpParolaAnimate();
      // Parola rewrites every zone; put the name pixels back after number anims.
      if (slot == AnimSlot::Num) restoreNameBlit();
      pollDuringAnim();
    }
    endParolaPump();
    if (slot == AnimSlot::Num) restoreNameBlit();
    return animCurrent(slot, gen);
  }

  bool runEffectAnim(uint8_t zone, const char* text, uint8_t effect,
                     uint16_t speed, uint16_t pauseMs, uint32_t gen, AnimSlot slot) {
    if (!animCurrent(slot, gen)) return false;

    const uint8_t resolved = resolveEffect(effect);
    if (ShowMode::isLaserVert(resolved)) {
      // Number-only (inc/dec) or name-only single-zone boards.
      const uint32_t nameGen = (slot == AnimSlot::Name) ? gen : nameAnimGen;
      const uint32_t numGen = (slot == AnimSlot::Num) ? gen : numAnimGen;
      const int8_t nz =
          (slot == AnimSlot::Name) ? static_cast<int8_t>(zone) : static_cast<int8_t>(-1);
      const int8_t cz =
          (slot == AnimSlot::Num) ? static_cast<int8_t>(zone) : static_cast<int8_t>(-1);
      if (!laserVertWipeZones(nz, (slot == AnimSlot::Name) ? text : nullptr, cz,
                              (slot == AnimSlot::Num) ? text : nullptr,
                              resolved == ShowMode::LASER_UP, true, nameGen, numGen)) {
        return false;
      }
      if (slot == AnimSlot::Num) restoreNameBlit();
      return animCurrent(slot, gen);
    }
    if (resolved == ShowMode::PILING) {
      const uint32_t nameGen = (slot == AnimSlot::Name) ? gen : nameAnimGen;
      const uint32_t numGen = (slot == AnimSlot::Num) ? gen : numAnimGen;
      const int8_t nz =
          (slot == AnimSlot::Name) ? static_cast<int8_t>(zone) : static_cast<int8_t>(-1);
      const int8_t cz =
          (slot == AnimSlot::Num) ? static_cast<int8_t>(zone) : static_cast<int8_t>(-1);
      if (!pilingSandZones(nz, (slot == AnimSlot::Name) ? text : nullptr, cz,
                           (slot == AnimSlot::Num) ? text : nullptr, true, nameGen, numGen)) {
        return false;
      }
      if (slot == AnimSlot::Num) restoreNameBlit();
      return animCurrent(slot, gen);
    }
    if (resolved == ShowMode::ANIMATION) {
      const uint32_t nameGen = (slot == AnimSlot::Name) ? gen : nameAnimGen;
      const uint32_t numGen = (slot == AnimSlot::Num) ? gen : numAnimGen;
      const int8_t nz =
          (slot == AnimSlot::Name) ? static_cast<int8_t>(zone) : static_cast<int8_t>(-1);
      const int8_t cz =
          (slot == AnimSlot::Num) ? static_cast<int8_t>(zone) : static_cast<int8_t>(-1);
      if (!animationRandomZones(nz, (slot == AnimSlot::Name) ? text : nullptr, cz,
                                (slot == AnimSlot::Num) ? text : nullptr, true, nameGen,
                                numGen)) {
        return false;
      }
      if (slot == AnimSlot::Num) restoreNameBlit();
      return animCurrent(slot, gen);
    }
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
      spd = 0;  // pump bursts pixels; tick unused
    }
    return runZoneAnim(zone, text, inEff, PA_NO_EFFECT, spd, pauseMs, gen, slot, resolved);
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

  void runCounterEnterAnim(uint8_t effect, uint8_t speed) {
    board->displayShutdown(false);
    board->setIntensity(effectiveIntensity());

    const uint8_t eff = resolveEffect(effect);
    useEffectSpeed(eff, speed);
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

  void runCountIncAnim(int32_t newCount, uint8_t effect, uint8_t speed) {
    board->displayShutdown(false);
    board->setIntensity(effectiveIntensity());
    paintNameOnly();
    const uint32_t gen = beginAnim(AnimSlot::Num);
    const uint8_t eff = resolveEffect(effect);
    useEffectSpeed(eff, speed);
    bool ok;
    if (isMilestone(newCount)) {
      ok = playFireworks(newCount, gen);
    } else {
      ok = playCountAnim(newCount, eff, gen);
    }
    if (ok) restoreBlankAfterAnim();
  }

  void runCountDecAnim(int32_t newCount, uint8_t effect, uint8_t speed) {
    board->displayShutdown(false);
    board->setIntensity(effectiveIntensity());
    paintNameOnly();
    const uint32_t gen = beginAnim(AnimSlot::Num);
    const uint8_t eff = resolveEffect(effect);
    useEffectSpeed(eff, speed);
    if (playCountAnim(newCount, eff, gen)) {
      restoreBlankAfterAnim();
    }
  }

  void clearScrollerCycle() {
    scrollerCycleAll = false;
    scrollerHolding = false;
    scrollerAdvancing = false;
  }

  int8_t nextNonEmptyScrollMsg(uint8_t from) {
    const uint8_t n = Counters::scrollMessageCount();
    if (n == 0) return -1;
    for (uint8_t step = 0; step < n; step++) {
      const uint8_t i = static_cast<uint8_t>((from + step) % n);
      const char* msg = Counters::scrollMessage(i);
      if (msg && msg[0]) return static_cast<int8_t>(i);
    }
    return -1;
  }

  void fillScrollerBuffers(uint8_t msgIdx) {
    const char* msg = Counters::scrollMessage(msgIdx);
    if (msg && msg[0]) {
      strncpy(scrollerBuf, msg, SCROLLER_MAX_LEN);
      scrollerBuf[SCROLLER_MAX_LEN] = '\0';
    } else {
      strncpy(scrollerBuf, " ", sizeof(scrollerBuf));
    }
    const char* bot = Counters::scrollMessageBottom(msgIdx);
    if (bot && bot[0]) {
      strncpy(scrollerBottomBuf, bot, SCROLLER_BOTTOM_MAX_LEN);
      scrollerBottomBuf[SCROLLER_BOTTOM_MAX_LEN] = '\0';
    } else {
      scrollerBottomBuf[0] = '\0';
    }
  }

  void setScrollerDirs(uint8_t effectIn) {
    const uint8_t resolvedIn = resolveEffect(effectIn);
    useMarqueeSpeed();
    const bool wantLeft = (resolvedIn != ShowMode::RIGHT);
    bool nameParolaRight = !wantLeft;
    if (NAME_ZONE_FLIP_LR) nameParolaRight = !nameParolaRight;
    scrollerDir = nameParolaRight ? PA_SCROLL_RIGHT : PA_SCROLL_LEFT;
    scrollerBottomDir = wantLeft ? PA_SCROLL_LEFT : PA_SCROLL_RIGHT;
  }

  // Assumes scrollerBuf / bottom / dirs already set.
  bool armScrollerMarquee() {
    const int8_t nz = nameZoneId();
    const int8_t cz = numberZone();
    if (nz < 0) return false;

    scrollerZone = static_cast<uint8_t>(nz);
    scrollerHasBottom = (cz >= 0 && scrollerBottomBuf[0]);
    scrollerBottomZone = scrollerHasBottom ? static_cast<uint8_t>(cz) : 0;

    uint8_t fitBuf[NAME_COL_BUF];
    // fitBuf only decides scroll-vs-static; measure full width for pass timing.
    const uint16_t topFitCols = buildNameColumns(scrollerBuf, fitBuf, NAME_COL_BUF);
    const uint16_t topZoneCols = zoneWidthCols(scrollerZone);
    const uint16_t topCols = measureTextColumns(scrollerBuf);
    scrollerTopScrolls = (topCols > topZoneCols) || (topFitCols > topZoneCols);

    uint16_t botCols = 0;
    uint16_t botZoneCols = 0;
    scrollerBottomScrolls = false;
    if (scrollerHasBottom) {
      uint8_t botFit[64];
      (void)buildNameColumns(scrollerBottomBuf, botFit, sizeof(botFit));
      botZoneCols = zoneWidthCols(scrollerBottomZone);
      botCols = measureTextColumns(scrollerBottomBuf);
      scrollerBottomScrolls = (botCols > botZoneCols);
    }

    // Parola HScroll IN≈textCols + OUT≈zoneCols. Scale bottom tick so both
    // passes finish together (long top → much slower bottom).
    scrollerTopSpd = parolaScrollSpeed(40);
    scrollerBotSpd = scrollerTopSpd;
    if (scrollerTopScrolls && scrollerBottomScrolls) {
      const uint32_t topDist = static_cast<uint32_t>(topCols) + topZoneCols;
      const uint32_t botDist = static_cast<uint32_t>(botCols) + botZoneCols;
      if (botDist > 0 && topDist > botDist) {
        uint32_t scaled =
            (static_cast<uint32_t>(scrollerTopSpd) * topDist + (botDist / 2)) /
            botDist;
        if (scaled < scrollerTopSpd) scaled = scrollerTopSpd;
        if (scaled > 2000) scaled = 2000;
        scrollerBotSpd = static_cast<uint16_t>(scaled);
      }
    }

    scrollerOn = true;
    scrollerHolding = false;
    scrollerTopPassLatched = !scrollerTopScrolls;
    scrollerBotPassLatched = !scrollerBottomScrolls;
    scrollerPassDone = scrollerTopPassLatched && scrollerBotPassLatched;

    board->displayShutdown(false);
    board->setIntensity(effectiveIntensity());
    board->displayClear();

    board->setCharSpacing(scrollerZone, 1);
    if (scrollerTopScrolls) {
      board->displayZoneText(scrollerZone, scrollerBuf, PA_LEFT, scrollerTopSpd, 0,
                             scrollerDir, scrollerDir);
    } else {
      board->displayZoneText(scrollerZone, scrollerBuf, PA_CENTER, 0, 0, PA_PRINT,
                             PA_NO_EFFECT);
    }
    board->displayReset(scrollerZone);

    if (cz >= 0) {
      board->displayClear(cz);
      if (scrollerHasBottom) {
        board->setCharSpacing(scrollerBottomZone, 1);
        if (scrollerBottomScrolls) {
          board->displayZoneText(scrollerBottomZone, scrollerBottomBuf, PA_LEFT,
                                 scrollerBotSpd, 0, scrollerBottomDir,
                                 scrollerBottomDir);
        } else {
          board->displayZoneText(scrollerBottomZone, scrollerBottomBuf, PA_CENTER, 0,
                                 0, PA_PRINT, PA_NO_EFFECT);
        }
        board->displayReset(scrollerBottomZone);
      }
    }
    return true;
  }

  void restartScrollerPass() {
    scrollerTopPassLatched = !scrollerTopScrolls;
    scrollerBotPassLatched = !scrollerBottomScrolls;
    scrollerPassDone = false;
    scrollerHolding = false;
    if (scrollerTopScrolls) {
      board->displayClear(scrollerZone);
      board->setCharSpacing(scrollerZone, 1);
      board->displayZoneText(scrollerZone, scrollerBuf, PA_LEFT, scrollerTopSpd, 0,
                             scrollerDir, scrollerDir);
      board->displayReset(scrollerZone);
    }
    if (scrollerBottomScrolls && scrollerHasBottom) {
      board->displayClear(scrollerBottomZone);
      board->setCharSpacing(scrollerBottomZone, 1);
      board->displayZoneText(scrollerBottomZone, scrollerBottomBuf, PA_LEFT,
                             scrollerBotSpd, 0, scrollerBottomDir, scrollerBottomDir);
      board->displayReset(scrollerBottomZone);
    }
  }

  void advanceScrollerCycle() {
    if (!scrollerCycleAll || scrollerAdvancing || !board) return;
    scrollerAdvancing = true;

    const uint8_t n = Counters::scrollMessageCount();
    int8_t next = (n > 0)
                      ? nextNonEmptyScrollMsg(
                            static_cast<uint8_t>((scrollerCycleIdx + 1) % n))
                      : static_cast<int8_t>(-1);
    if (next < 0) {
      scrollerAdvancing = false;
      return;
    }

    // Sole scrolling message: loop the same marquee pass.
    if (static_cast<uint8_t>(next) == scrollerCycleIdx &&
        (scrollerTopScrolls || scrollerBottomScrolls)) {
      restartScrollerPass();
      scrollerAdvancing = false;
      return;
    }
    // Sole static message: leave it up until the user stops.
    if (static_cast<uint8_t>(next) == scrollerCycleIdx) {
      scrollerAdvancing = false;
      return;
    }

    const bool fromScroller = scrollerOn;
    scrollerOn = false;
    scrollerTopScrolls = false;
    scrollerBottomScrolls = false;
    stopNameTeeter();
    nameStaticValid = false;
    const uint32_t nameGen = beginAnim(AnimSlot::Name);
    const uint32_t numGen = beginAnim(AnimSlot::Num);
    const uint8_t resolvedOut = resolveEffect(scrollerCycleOutEff);
    useEffectSpeed(resolvedOut, scrollerCycleSpeed);
    if (!scrollDisplayedOff(resolvedOut, nameGen, numGen, fromScroller)) {
      clearScrollerCycle();
      scrollerAdvancing = false;
      return;
    }

    scrollerCycleIdx = static_cast<uint8_t>(next);
    fillScrollerBuffers(scrollerCycleIdx);
    setScrollerDirs(scrollerCycleInEff);
    if (!armScrollerMarquee()) {
      clearScrollerCycle();
    }
    scrollerAdvancing = false;
  }

  void drainPendingAnims() {
    while (pendingAnim.kind != PendingKind::None) {
      const PendingAnim job = pendingAnim;
      pendingAnim.kind = PendingKind::None;
      animBusy = true;
      switch (job.kind) {
        case PendingKind::CounterEnter:
          runCounterEnterAnim(job.effect, job.speed);
          break;
        case PendingKind::CountInc:
          runCountIncAnim(job.count, job.effect, job.speed);
          break;
        case PendingKind::CountDec:
          runCountDecAnim(job.count, job.effect, job.speed);
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

// Forward — used by idleShowNext / stopManualScroller / boot splash
void haltScrollerEngine();
void startManualScroller(uint8_t effectIn, const char* text, const char* bottom,
                         uint8_t effectOut, uint8_t speed);
void stopManualScroller(bool restore);

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
}

void showStatusLines(const char* top, const char* bottom) {
  if (!inited || !board) return;
  clearTestPattern();
  if (scrollerOn) {
    haltScrollerEngine();
  }
  stopNameTeeter();
  stopBotTeeter();
  nameStaticValid = false;
  beginAnim(AnimSlot::Name);
  beginAnim(AnimSlot::Num);

  board->displayShutdown(false);
  board->setIntensity(effectiveIntensity());
  board->displayClear();

  const int8_t nz = nameZoneId();
  const int8_t cz = numberZone();
  const char* t = (top && top[0]) ? top : " ";
  const char* b = (bottom && bottom[0]) ? bottom : " ";

  // Bottom first (same reason as paintContent), then top — teeter if cut off.
  if (cz >= 0) {
    paintBotLine(static_cast<uint8_t>(cz), b);
  }
  if (nz >= 0) {
    paintName(static_cast<uint8_t>(nz), t);
  }
}

void playBootSplash() {
  if (!inited || !board) return;
  IdleCycle::noteActivity();
  playBootAnimation();

  constexpr uint32_t kBudgetMs = 10000;
  constexpr uint32_t kHoldMs = 2000;
  const uint32_t tStart = millis();

  auto abortOrBudget = [&]() -> bool {
    uint8_t btn = 0;
    if (WizRemote::takeButton(btn)) return true;
    return (millis() - tStart) >= kBudgetMs;
  };

  auto showPage = [&](const char* top, const char* bot) -> bool {
    if (abortOrBudget()) return true;

    char topBuf[SCROLLER_MAX_LEN + 1];
    char botBuf[SCROLLER_BOTTOM_MAX_LEN + 1];
    strncpy(topBuf, top ? top : " ", SCROLLER_MAX_LEN);
    topBuf[SCROLLER_MAX_LEN] = '\0';
    strncpy(botBuf, bot ? bot : " ", SCROLLER_BOTTOM_MAX_LEN);
    botBuf[SCROLLER_BOTTOM_MAX_LEN] = '\0';

    showStatusLines(topBuf, botBuf);

    const uint32_t pageStart = millis();
    while ((millis() - pageStart) < kHoldMs) {
      if (abortOrBudget()) {
        stopNameTeeter();
        stopBotTeeter();
        return true;
      }
      WifiMgr::loop();
      tickNameTeeter();
      tickBotTeeter();
      yield();
    }
    return false;
  };

  char top[24];
  char bot[SCROLLER_BOTTOM_MAX_LEN + 1];

  // SoftAP: IP on wide top, SSID on bottom.
  {
    String ip = WifiMgr::apIp();
    snprintf(top, sizeof(top), "%s", ip.c_str());
    if (showPage(top, AP_SSID)) goto done;
  }

  // STA: IP top, network name bottom.
  if (WifiMgr::staConnected()) {
    String sip = WifiMgr::staIp();
    snprintf(top, sizeof(top), "%s", sip.c_str());
    const char* ssid = WifiMgr::staSsid();
    if (showPage(top, (ssid && ssid[0]) ? ssid : "STA")) goto done;
  } else {
    if (showPage("-", "none")) goto done;
  }

  {
    const char* linked = Counters::linkedRemoteMac();
    const char* seen = WizRemote::lastSeenMac();
    const char* id = (linked && linked[0]) ? linked
                     : (seen && seen[0])   ? seen
                                           : "-";
    if (showPage(id, "remote")) goto done;
  }

  {
    snprintf(bot, sizeof(bot), "%u on", static_cast<unsigned>(Counters::enabledCount()));
    const Counter& c = Counters::getConst(Counters::activeIndex());
    if (c.name[0]) {
      if (showPage(c.name, bot)) goto done;
    } else {
      if (showPage("Counters", bot)) goto done;
    }
  }

done:
  if (scrollerOn) stopManualScroller(false);
  IdleCycle::noteActivity();
  Serial.println(F("[display] boot splash done"));
}

void showTestAllOn() {
  if (!inited || !board) return;
  if (scrollerOn) haltScrollerEngine();
  stopNameTeeter();
  stopBotTeeter();
  nameStaticValid = false;
  beginAnim(AnimSlot::Name);
  beginAnim(AnimSlot::Num);

  testPattern = TestPattern::AllOn;
  board->displayShutdown(false);
  board->setIntensity(Counters::intensity());

  MD_MAX72XX* mx = board->getGraphicObject();
  if (!mx) return;
  const uint16_t cols = static_cast<uint16_t>(MODULES_PER_BOARD) * 8u;
  for (uint16_t c = 0; c < cols; c++) {
    mx->setColumn(c, 0xFF);
  }
  mx->update();
}

void showTestCheckerboard() {
  if (!inited || !board) return;
  if (scrollerOn) haltScrollerEngine();
  stopNameTeeter();
  stopBotTeeter();
  nameStaticValid = false;
  beginAnim(AnimSlot::Name);
  beginAnim(AnimSlot::Num);

  testPattern = TestPattern::Checker;
  checkerPhase = 0;
  checkerNextMs = millis() + CHECKER_PERIOD_MS;
  board->displayShutdown(false);
  board->setIntensity(Counters::intensity());

  MD_MAX72XX* mx = board->getGraphicObject();
  if (!mx) return;
  const uint16_t cols = static_cast<uint16_t>(MODULES_PER_BOARD) * 8u;
  for (uint16_t c = 0; c < cols; c++) {
    uint8_t bits = 0;
    for (uint8_t r = 0; r < 8; r++) {
      // 2×2 checker cells; phase flips black/white.
      if ((((c >> 1) + (r >> 1) + checkerPhase) & 1u) == 0u) {
        bits = static_cast<uint8_t>(bits | (1u << r));
      }
    }
    mx->setColumn(c, bits);
  }
  mx->update();
}

void loop() {
  if (!inited || !board) return;

  if (flashOn && millis() >= flashUntil) {
    flashOn = false;
    board->setInvert(false);
    applyBlankState();
  }

  if (testPattern == TestPattern::Checker) {
    if (static_cast<int32_t>(millis() - checkerNextMs) >= 0) {
      checkerPhase ^= 1u;
      checkerNextMs = millis() + CHECKER_PERIOD_MS;
      MD_MAX72XX* mx = board->getGraphicObject();
      if (mx) {
        const uint16_t cols = static_cast<uint16_t>(MODULES_PER_BOARD) * 8u;
        for (uint16_t c = 0; c < cols; c++) {
          uint8_t bits = 0;
          for (uint8_t r = 0; r < 8; r++) {
            if ((((c >> 1) + (r >> 1) + checkerPhase) & 1u) == 0u) {
              bits = static_cast<uint8_t>(bits | (1u << r));
            }
          }
          mx->setColumn(c, bits);
        }
        mx->update();
      }
    }
    return;
  }
  if (testPattern == TestPattern::AllOn) {
    return;
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

    // Night / web Start: rotate through all messages until toggled off.
    if (scrollerCycleAll && scrollerPassDone && !scrollerAdvancing) {
      const bool needsHold = !scrollerTopScrolls && !scrollerBottomScrolls;
      if (needsHold) {
        if (!scrollerHolding) {
          scrollerHolding = true;
          scrollerHoldStartMs = millis();
        }
        const uint32_t holdMs =
            static_cast<uint32_t>(Counters::idleCycleSeconds()) * 1000UL;
        if (static_cast<uint32_t>(millis() - scrollerHoldStartMs) < holdMs) {
          return;
        }
      }
      advanceScrollerCycle();
    }
    return;
  }

  if (teeter.active || botTeeter.active) {
    // Direct column blit; avoid Parola animate (it would overwrite teeter pixels).
    tickNameTeeter();
    tickBotTeeter();
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

void onCountIncreased(int32_t newCount, uint8_t effect, uint8_t speed) {
  scrollerOn = false;
  if (!inited || !board || !displayVisible()) {
    applyBlankState();
    return;
  }

  pendingAnim.kind = PendingKind::CountInc;
  pendingAnim.effect = effect;
  pendingAnim.speed = speed;
  pendingAnim.count = newCount;
  cancelInFlightAnims();
  if (animBusy) return;
  drainPendingAnims();
}

void onCountDecreased(int32_t newCount, uint8_t effect, uint8_t speed) {
  scrollerOn = false;
  if (!inited || !board || !displayVisible()) {
    applyBlankState();
    return;
  }

  pendingAnim.kind = PendingKind::CountDec;
  pendingAnim.effect = effect;
  pendingAnim.speed = speed;
  pendingAnim.count = newCount;
  cancelInFlightAnims();
  if (animBusy) return;
  drainPendingAnims();
}

void idleShowNext(int32_t /*count*/, uint8_t effect, uint8_t speed) {
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
  pendingAnim.speed = speed;
  pendingAnim.count = 0;
  cancelInFlightAnims();
  if (animBusy) return;
  drainPendingAnims();
}

void selectShow(uint8_t effect, uint8_t speed) {
  idleShowNext(0, effect, speed);
}

bool scrollerActive() { return scrollerOn; }

bool scrollerCompletedPass() { return scrollerPassDone; }

// Halt marquee and leave Parola zones idle so loop() won't keep scrolling.
void haltScrollerEngine() {
  clearScrollerCycle();
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

bool animateDisplayedOff(uint8_t effect, uint8_t speed) {
  if (!inited || !board) return false;
  const bool fromScroller = scrollerOn;
  // Stop marquee engine; scroller exit must not reprint text.
  clearScrollerCycle();
  scrollerOn = false;
  scrollerPassDone = false;
  scrollerTopScrolls = false;
  scrollerBottomScrolls = false;
  stopNameTeeter();
  nameStaticValid = false;
  const uint32_t nameGen = beginAnim(AnimSlot::Name);
  const uint32_t numGen = beginAnim(AnimSlot::Num);
  const uint8_t resolved = resolveEffect(effect);
  useEffectSpeed(resolved, speed);
  if (!scrollDisplayedOff(resolved, nameGen, numGen, fromScroller)) return false;
  board->displayClear();
  return true;
}

void startManualScroller(uint8_t effectIn, const char* text, const char* bottom,
                         uint8_t effectOut, uint8_t speed) {
  if (!inited || !board) return;
  if (!displayVisible()) {
    applyBlankState();
    return;
  }

  // nullptr text → Night / web Start: cycle every non-empty scrollMessage.
  // Explicit text → idle playlist one-shot for that message.
  const bool cycleAll = (text == nullptr);

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
  useEffectSpeed(resolvedOut, speed);

  // Exit previous content (scroller never reprints — that flashed text after scroll-off).
  if (!scrollDisplayedOff(resolvedOut, nameGen, numGen, fromScroller)) {
    clearScrollerCycle();
    return;
  }

  if (cycleAll) {
    const int8_t first = nextNonEmptyScrollMsg(0);
    if (first < 0) {
      clearScrollerCycle();
      board->displayClear();
      return;
    }
    scrollerCycleAll = true;
    scrollerCycleIdx = static_cast<uint8_t>(first);
    scrollerCycleInEff = effectIn;
    scrollerCycleOutEff = effectOut;
    scrollerCycleSpeed = speed;
    fillScrollerBuffers(scrollerCycleIdx);
  } else {
    clearScrollerCycle();
    if (text[0]) {
      strncpy(scrollerBuf, text, SCROLLER_MAX_LEN);
      scrollerBuf[SCROLLER_MAX_LEN] = '\0';
    } else {
      strncpy(scrollerBuf, " ", sizeof(scrollerBuf));
    }
    if (bottom && bottom[0]) {
      strncpy(scrollerBottomBuf, bottom, SCROLLER_BOTTOM_MAX_LEN);
      scrollerBottomBuf[SCROLLER_BOTTOM_MAX_LEN] = '\0';
    } else {
      scrollerBottomBuf[0] = '\0';
    }
  }

  setScrollerDirs(effectIn);
  if (!armScrollerMarquee()) {
    clearScrollerCycle();
  }
}

}  // namespace Display
