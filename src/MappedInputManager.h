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
  // Right-key hold that is Select outside a book -- the power button, whose tap
  // opens the control centre and so is the way back in, and the capacitive Home
  // key.
  static bool isTouchInputEnabled() { return touchInputEnabled; }
  static void setTouchInputEnabled(const bool enabled) { touchInputEnabled = enabled; }

  // --- In-book context ---------------------------------------------------------
  // The input scheme is context-sensitive, and this is the switch.
  //
  //   inside a book   Left = page back, Right = page forward, and NOTHING else.
  //                   No long presses, no chord: the side keys are page keys,
  //                   they fire on the press edge with no window to wait out,
  //                   and a reader has to be able to lean on them.
  //   outside a book  Left / Right step the selection, a Left HOLD is Back and a
  //                   a Right HOLD is Select -- the two roles the X4 Pro has no
  //                   keys for. This costs the side keys their press-edge
  //                   dispatch (see updateSideGestures), but only on the screens
  //                   where nobody is turning pages.
  //
  // "Inside a book" means the reader PAGE is the focused activity. A menu, a
  // popup or the control centre pushed on top of the reader is OUTSIDE: those
  // are exactly the screens that need Back and Select. Set from main.cpp each
  // frame, immediately before update().
  static void setInBookContext(const bool inBook) { inBookContext = inBook; }
  static bool isInBookContext() { return inBookContext; }

  // --- Control-centre routing --------------------------------------------------
  // One-frame events the power button publishes, routed by ActivityManager.
  // Open = a power tap while the short-click action is PWR_CONTROL_CENTER;
  // close = a ~1 s power hold. Latched on read so a re-entrant
  // ActivityManager::loop() cannot act on the same gesture twice; the latch
  // clears when main.cpp clears the frame at the top of the next loop.
  bool consumeControlCenterOpen() const;
  bool consumeControlCenterClose() const;
#if FREEINK_CAP_TOUCH
  // --- One-frame power-button gesture events ---------------------------------
  // On boards running the power-gesture scheme (see
  // CrossPointSettings::usesPowerGestures) a press is classified by main.cpp's
  // decoder, the only place that knows how long the button was down and whether
  // a second tap followed. It publishes the outcome here for exactly one frame;
  // MappedInputManager swallows the raw release on those boards so no consumer
  // can see the same press twice.
  //
  // The click frame stands in for the release the decoder swallowed, so every
  // short-power-click consumer (page turn, force refresh, footnotes, and Confirm
  // if the user has bound it there) keeps working unchanged -- one double-tap
  // window later than the finger lift, which is the price of having the double
  // tap back. See CrossPointSettings::POWER_DOUBLE_TAP_MS for why that price is
  // affordable on this action when it was not on the ones it replaced.
  void setPowerClickFrame(const bool clicked) const {
    powerClickFrame = clicked;
    if (!clicked) controlCenterOpenLatched = false;
  }
  // A ~1 s power hold: closes the control centre.
  void setPowerCloseFrame(const bool closed) const {
    powerCloseFrame = closed;
    if (!closed) controlCenterCloseLatched = false;
  }
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

  // --- Side-key long presses (outside a book only) -----------------------------
  // A key cannot be both "step the selection" on its press edge and "Back /
  // Select" on a hold: by the time the hold is knowable the step has already
  // happened, and selecting the row you were forced to move off is not a usable
  // Select. So outside a book the side keys are resolved on the RELEASE:
  //
  //   press edge                 parked, nothing published
  //   held >= SIDE_LONG_PRESS_MS Back (Left) or Select (Right) fires there and
  //                              then, while the key is still down, and the
  //                              parked press is discarded
  //   release before that        the parked press is published on the release
  //                              frame and a synthetic release on the next one,
  //                              so no consumer sees both edges in one frame
  //
  // The long press fires AT the threshold rather than on the release because
  // nothing longer competes with it -- there is no sleep hold on these keys --
  // so the user gets the action the moment it is earned.
  //
  // Cost: a navigation step now lands on the finger LIFT rather than the press,
  // i.e. however long the tap lasted, typically well under 150 ms and invisible
  // behind a panel repaint. Continuous auto-repeat goes with it (isPressed()
  // reads false for a parked key, so ButtonNavigator's repeat never arms) --
  // which is required, not incidental: a held key means Back or Select now.
  //
  // Inside a book NONE of this runs and readKey() is a straight passthrough:
  // page turns keep their press-edge dispatch and their auto-repeat.
  //
  // Armed only on boards with no physical Back/Confirm key
  // (CrossPointSettings::usesPowerGestures); no other board pays for roles its
  // keys already cover.
  void updateSideGestures() const;
  // The single point where BTN_UP / BTN_DOWN are read, so the gesture machine
  // can swallow and republish their edges in one place.
  bool readKey(uint8_t index, bool (HalGPIO::*fn)(uint8_t) const) const;
  static uint8_t sideKeyMask(const uint8_t index) {
    return index == HalGPIO::BTN_UP ? 0x1u : (index == HalGPIO::BTN_DOWN ? 0x2u : 0u);
  }
  static bool usesSideGestures();

  static bool touchInputEnabled;
  static bool inBookContext;

  mutable bool touchHeldOverrideValid = false;
  mutable unsigned long touchHeldOverrideMs = 0;
  mutable unsigned long touchHeldOverrideAt = 0;
  mutable uint16_t longPressFiredButtons = 0;
  mutable uint16_t suppressedReleaseButtons = 0;
  // Per-contact state for the machine above. Index 0 = BTN_UP (physical Left),
  // index 1 = BTN_DOWN (physical Right). `parked` is set at the press edge and
  // says this contact belongs to the machine; `consumed` says its long press has
  // already fired, so the rest of the contact is swallowed outright.
  mutable bool sideKeyDown[2] = {false, false};
  mutable bool sideKeyParked[2] = {false, false};
  mutable bool sideKeyConsumed[2] = {false, false};
  mutable unsigned long sideKeyDownAt[2] = {0, 0};
  // One-frame long-press outcomes, folded into logical Back / Confirm below.
  mutable bool sideBackFrame = false;
  mutable bool sideSelectFrame = false;
  // Per-frame edge bookkeeping readKey() consults. Bit 0 = BTN_UP, bit 1 =
  // BTN_DOWN. Emit* are edges the machine publishes itself; hide* are raw gpio
  // edges it hides; sideReleaseDue is the synthetic release parked for the next
  // frame. sideHideHeld keeps a held key out of isPressed(), so a hold on its
  // way to Back / Select cannot also drive ButtonNavigator's auto-repeat.
  mutable uint8_t sideEmitPress = 0;
  mutable uint8_t sideEmitRelease = 0;
  mutable uint8_t sideReleaseDue = 0;
  mutable uint8_t sideHidePress = 0;
  mutable uint8_t sideHideRelease = 0;
  mutable uint8_t sideHideHeld = 0;
  mutable bool controlCenterOpenLatched = false;
  mutable bool controlCenterCloseLatched = false;
#if FREEINK_CAP_TOUCH
  mutable bool powerClickFrame = false;
  mutable bool powerCloseFrame = false;
#endif
};
