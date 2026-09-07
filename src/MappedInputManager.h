#pragma once

#include <HalGPIO.h>

class GfxRenderer;
namespace freeink {
namespace ui {
enum class ScreenEdge : uint8_t;
}
}  // namespace freeink

class MappedInputManager {
 public:
  enum class Button {
    Back,
    Confirm,
    Left,
    Right,
    Up,
    Down,
    Power,
    PageBack,
    PageForward,
    NavNext,
    NavPrevious,
    ScreenLeft,
    ScreenRight,
    ScreenUp,
    ScreenDown
  };
  enum class SwipeDir { None, Left, Right, Up, Down };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  MappedInputManager(HalGPIO& gpio, const GfxRenderer& renderer) : gpio(gpio), renderer(renderer) {}

  void update() const;

  // --- Touchscreen master gate ------------------------------------------------
  // Runtime-only (never persisted): a board whose glass is switched off must
  // always come back with touch alive after a reboot or a wake, otherwise a
  // touch-first device like the X4 Pro can lock its owner out of the UI. Driven
  // from the control centre's Touch tile (and Settings -> Controls ->
  // Touchscreen, which is the same gate). Deliberately NOT gated, so the device
  // stays navigable with the glass dead: the two side keys -- including the
  // Left+Right chord that opens the control centre, which is the way back in --
  // and the capacitive Home key.
  static bool isTouchInputEnabled() { return touchInputEnabled; }
  static void setTouchInputEnabled(const bool enabled) { touchInputEnabled = enabled; }

  // --- Left + Right chord ------------------------------------------------------
  // One-frame event: both side keys went down together (see updateSideCombo).
  // It toggles the control centre, which is the button-only route back to the
  // touch switch after the glass has been turned off. Consumed on read so a
  // re-entrant ActivityManager::loop() cannot act on the same chord twice.
  bool consumeControlCenterChord() const {
    const bool fired = sideComboFrame;
    sideComboFrame = false;
    return fired;
  }
#if FREEINK_CAP_TOUCH
  // --- One-frame power-button gesture events ---------------------------------
  // On boards running the two-gesture power scheme (see
  // CrossPointSettings::usesPowerGestures) a press is only classified once the
  // button comes back up: a release under the click ceiling is a tap, a release
  // past the Back floor is Back, and the band between them is inert. main.cpp's
  // decoder owns that classification and publishes the outcome here for exactly
  // one frame; MappedInputManager swallows the raw release on those boards so no
  // consumer can see the same press twice.
  //
  // The click frame stands in for the release itself and is published on the
  // very frame the finger lifts, so every existing short-power-click consumer
  // (Confirm, page turn, force refresh, footnotes) keeps working unchanged and
  // with no added latency.
  void setPowerClickFrame(const bool clicked) { powerClickFrame = clicked; }
  // A ~1 s power hold, folded into logical Button::Back below.
  void setPowerBackFrame(const bool back) { powerBackFrame = back; }
#endif
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  // One-shot threshold event while the button is down; consumes its release.
  bool wasLongPressed(Button button, unsigned long thresholdMs) const;
  bool consumeSuppressedRelease() const;
  bool isPressed(Button button) const;
  bool hasTouch() const;
  bool wasScreenTapped(int& x, int& y) const;
  bool wasScreenTouchDown(int& x, int& y) const;
  // One-shot long-press from the SDK touch classifier, fired WHILE the finger
  // is still down (stationary contact held past the SDK threshold). Consuming
  // it suppresses the remainder of the contact — its continued hold and its
  // release edge — so the ensuing finger lift can't also tap-dismiss the popup
  // the long-press opened. The SDK owns that latch and self-clears it once the
  // contact ends.
  bool wasScreenLongPress(int& x, int& y) const;
  bool isScreenTouchHeld(int& x, int& y) const;
  // Raw release edge, also true when the contact ended in a swipe or drag-off
  // (which wasScreenTapped never reports). InputSnapshot builders forward it
  // off-target so FreeInkUI routing clears its pressed-element state.
  bool wasScreenTouchReleased() const;
  bool wasTapInRect(int x, int y, int width, int height) const;
  // Tap on the persistent on-screen Back chip the themes paint on touch boards
  // (see BaseTheme::drawTouchBackButton). Folded into Button::Back below so
  // every activity's existing Back handling picks it up unchanged.
  bool wasBackButtonTap() const;

  // Combined touch interaction for a band of equal rows with caller-supplied
  // geometry — the shared hit-test for lists the theme helpers above do not
  // cover (custom row heights, option prompts, menus). Down = a held
  // tap-candidate is on a row (update the selection highlight); Tap = a tap
  // released on one (activate). rowHeight limits the hit to the top rowHeight
  // px of each step (0 = the full step, no gap band).
  enum class RowTouch : uint8_t { None, Down, Tap };
  RowTouch rowTouch(int& row, int top, int rowStep, int rowCount, int xStart = 0, int xEnd = INT32_MAX,
                    int rowHeight = 0) const;
  // Horizontal variant for side-by-side button pairs (confirmation prompts).
  RowTouch colTouch(int& col, int left, int colStep, int colCount, int yStart, int yEnd, int colWidth = 0) const;

  SwipeDir wasSwipe() const;
  // Back = left-to-right swipe anchored at the left edge. Public so swipe-mode
  // page turns (reader) can exclude it from a plain SwipeDir::Right.
  bool wasBackGesture() const;
  // Home-key boards use a short Home-key tap to exit; their bottom-edge swipe
  // is intentionally unused. Other boards retain the bottom-edge Home gesture.
  // The reader menu remains on its existing top-edge gesture and middle tap.
  bool wasHomeGesture() const;
  // A Home-key hold runs the configured long-press action in the reader.
  bool wasHomeKeyHold() const;
  bool wasMenuGesture() const;
  // Bottom-edge up-swipe as the reader-menu gesture (SHOW_READER_MENU's Swipe
  // Up option). Only meaningful on home-key boards, where Home lives on the
  // key and the bottom edge is free; elsewhere the same swipe is the Home
  // gesture and this returns false.
  bool wasReaderMenuSwipeUp() const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  const GfxRenderer& getRenderer() const { return renderer; }
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Maps four screen-direction labels onto the two physical front-button roles
  // using the same live-orientation transform as ScreenLeft/Right/Up/Down.
  Labels mapDirectionalLabels(const char* back, const char* confirm, const char* left, const char* right,
                              const char* up, const char* down) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

  // True when the control axis is flipped relative to the physical buttons: always on touch boards,
  // or when button-only boards opt in, while the screen is currently INVERTED / LANDSCAPE_CCW.
  [[nodiscard]] bool isNavDirectionSwapped() const;

 private:
  HalGPIO& gpio;
  // Logical-to-physical button mapping depends on what the user is actually looking at: when the
  // screen is rendered rotated, the directional buttons must flip to match. The renderer is the only
  // authority on the *live* orientation (the reader rotates it and restores portrait on exit), so we
  // read it here instead of CrossPointSettings.orientation, which is just the persisted reader
  // preference and stays "rotated" even while portrait UI like home/settings is on screen.
  const GfxRenderer& renderer;

  Button mapScreenDirection(Button button) const;
  Labels mapFrontLabels(const char* back, const char* confirm, const char* left, const char* right) const;
  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
  // SDK edge classification (fui::edgeSwipe) + the shared decode/held-time
  // bookkeeping; the wrappers below give each edge its board meaning.
  bool wasEdgeSwipe(freeink::ui::ScreenEdge edge) const;
  bool wasTopEdgeDownSwipe() const;
  bool wasBottomEdgeUpSwipe() const;
  // Fetch the pending swipe (if any) and map both endpoints to logical screen coords
  bool decodeSwipe(int& sx, int& sy, int& ex, int& ey) const;
#if FREEINK_CAP_TOUCH
  // The frame on which a short power click is dispatched: the decoder's event on
  // gesture boards, the raw release everywhere else.
  bool wasPowerShortClick() const;
  bool wasPowerConfirmClick() const;
#endif
  void rememberTouchHeldTime() const;
  void suppressNextRelease(Button button) const;

  // --- Left + Right chord detection --------------------------------------------
  // The two side keys are page up / page down, so a chord can only be told from
  // two page turns by WAITING: the first side key down starts a short window in
  // which neither key reports anything. If the other key joins inside the window
  // the chord fires and both keys stay swallowed for the rest of the contact; if
  // the window closes with one key still down, the parked press is republished
  // and everything carries on as an ordinary page turn, one window late. A key
  // that is released inside the window (a very fast tap) gets its parked press on
  // the release frame and a synthetic release on the next one, so no consumer
  // ever sees a press and a release in the same frame.
  //
  // Only armed on touch boards: they are the ones whose control centre the chord
  // opens, and no other board should pay the window as page-turn latency.
  static constexpr unsigned long SIDE_COMBO_WINDOW_MS = 80;
  enum class SideCombo : uint8_t { Idle, Armed, Chord, Replay };
  void updateSideCombo() const;
  // The single point where BTN_UP / BTN_DOWN are read, so the chord state
  // machine can swallow and republish their edges in one place.
  bool readKey(uint8_t index, bool (HalGPIO::*fn)(uint8_t) const) const;
  static uint8_t sideKeyMask(const uint8_t index) {
    return index == HalGPIO::BTN_UP ? 0x1u : (index == HalGPIO::BTN_DOWN ? 0x2u : 0u);
  }

  static bool touchInputEnabled;

  mutable bool touchHeldOverrideValid = false;
  mutable unsigned long touchHeldOverrideMs = 0;
  mutable unsigned long touchHeldOverrideAt = 0;
  mutable uint16_t longPressFiredButtons = 0;
  mutable uint16_t suppressedReleaseButtons = 0;
  mutable SideCombo sideComboState = SideCombo::Idle;
  mutable unsigned long sideComboArmedAt = 0;
  mutable uint8_t sideComboArmedKey = HalGPIO::BTN_UP;
  mutable bool sideComboFrame = false;
  // Side-key edges the chord machine republishes this frame, and the release it
  // has parked for the next one. Bit 0 = BTN_UP, bit 1 = BTN_DOWN.
  mutable uint8_t sideEmitPress = 0;
  mutable uint8_t sideEmitRelease = 0;
  mutable uint8_t sideReleaseDue = 0;
#if FREEINK_CAP_TOUCH
  bool powerClickFrame = false;
  bool powerBackFrame = false;
#endif
};
