#include "MappedInputManager.h"

#include <BoardConfig.h>
#include <FreeInkUICore.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"

namespace fui = freeink::ui;

// Runtime-only touchscreen gate; always starts enabled on boot/wake. See the
// header for why this is never persisted.
bool MappedInputManager::touchInputEnabled = true;

void MappedInputManager::update() const {
  gpio.update();
  // Before any consumer reads a button this frame: the chord machine decides
  // which side-key edges are visible at all (see readKey below).
  updateSideCombo();
  for (uint8_t value = 0; value <= static_cast<uint8_t>(Button::ScreenDown); ++value) {
    if (!isPressed(static_cast<Button>(value))) longPressFiredButtons &= ~(1u << value);
  }
}

// Left + Right chord detection. See the header for why a chord on these two
// keys has to be resolved by waiting rather than by looking at one frame.
void MappedInputManager::updateSideCombo() const {
  sideComboFrame = false;
  sideEmitPress = 0;
  // A release parked by the Replay state last frame is published now, on its own
  // frame, so its press (published on the previous one) is never seen alongside it.
  sideEmitRelease = sideReleaseDue;
  sideReleaseDue = 0;
  if (sideComboState == SideCombo::Replay) {
    sideComboState = SideCombo::Idle;
    return;
  }
  if (!gpio.hasTouch()) return;

  const bool up = gpio.isPressed(HalGPIO::BTN_UP);
  const bool down = gpio.isPressed(HalGPIO::BTN_DOWN);

  if (sideComboState == SideCombo::Chord) {
    // Both keys stay swallowed until the whole contact is over, so the lift of
    // the second finger cannot turn a page.
    if (!up && !down) sideComboState = SideCombo::Idle;
    return;
  }

  if (sideComboState == SideCombo::Armed) {
    if (up && down) {
      sideComboState = SideCombo::Chord;
      sideComboFrame = true;
      return;
    }
    const bool armedStillDown = sideComboArmedKey == HalGPIO::BTN_UP ? up : down;
    if (!armedStillDown) {
      // Pressed and released inside the window: a genuine, very fast tap. Give
      // it its press now and its release next frame.
      sideComboState = SideCombo::Replay;
      sideEmitPress = sideKeyMask(sideComboArmedKey);
      sideReleaseDue = sideKeyMask(sideComboArmedKey);
      return;
    }
    if (millis() - sideComboArmedAt >= SIDE_COMBO_WINDOW_MS) {
      // Nobody joined: a solo press after all, republished one window late.
      sideComboState = SideCombo::Idle;
      sideEmitPress = sideKeyMask(sideComboArmedKey);
    }
    return;
  }

  const bool upEdge = gpio.wasPressed(HalGPIO::BTN_UP);
  const bool downEdge = gpio.wasPressed(HalGPIO::BTN_DOWN);
  if (!upEdge && !downEdge) return;
  // Both edges in one frame, or one key arriving on top of a key that is already
  // down (the window closed on it first, so its page turn has already happened --
  // opening the panel is still the intent that matters).
  if ((upEdge && downEdge) || (upEdge && down) || (downEdge && up)) {
    sideComboState = SideCombo::Chord;
    sideComboFrame = true;
    return;
  }
  sideComboState = SideCombo::Armed;
  sideComboArmedAt = millis();
  sideComboArmedKey = upEdge ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN;
}

bool MappedInputManager::readKey(const uint8_t index, bool (HalGPIO::*fn)(uint8_t) const) const {
  const uint8_t bit = sideKeyMask(index);
  if (bit == 0) return (gpio.*fn)(index);
  if (fn == &HalGPIO::wasPressed && (sideEmitPress & bit) != 0) return true;
  if (fn == &HalGPIO::wasReleased && (sideEmitRelease & bit) != 0) return true;
  // Anything the chord machine has not republished itself is invisible while a
  // window is open, while the chord owns the contact, and on the replay frame.
  if (sideComboState != SideCombo::Idle) return false;
  return (gpio.*fn)(index);
}

bool MappedInputManager::isNavDirectionSwapped() const {
  // Touch boards always follow the rendered orientation; button-only boards keep the user toggle.
  // Home and settings render in portrait, so neither path swaps them.
  const auto orientation = renderer.getOrientation();
  return (gpio.hasTouch() || SETTINGS.frontButtonFollowOrientation) &&
         (orientation == GfxRenderer::PortraitInverted || orientation == GfxRenderer::LandscapeCounterClockwise);
}

MappedInputManager::Button MappedInputManager::mapScreenDirection(const Button button) const {
  // Rows follow GfxRenderer::Orientation's declared order.
  static constexpr Button directions[][4] = {
      {Button::Left, Button::Right, Button::Up, Button::Down},
      {Button::Down, Button::Up, Button::Left, Button::Right},
      {Button::Right, Button::Left, Button::Down, Button::Up},
      {Button::Up, Button::Down, Button::Right, Button::Left},
  };

  uint8_t direction = 0;
  switch (button) {
    case Button::ScreenLeft:
      direction = 0;
      break;
    case Button::ScreenRight:
      direction = 1;
      break;
    case Button::ScreenUp:
      direction = 2;
      break;
    case Button::ScreenDown:
      direction = 3;
      break;
    default:
      return button;
  }

  const uint8_t orientation =
      SETTINGS.frontButtonFollowOrientation ? static_cast<uint8_t>(renderer.getOrientation()) : 0;
  return directions[orientation][direction];
}

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const auto sideLayout = SETTINGS.sideButtonLayout;

  switch (button) {
    case Button::Back:
      // Logical Back maps to user-configured front button.
      return readKey(SETTINGS.frontButtonBack, fn);
    case Button::Confirm:
      // Logical Confirm maps to user-configured front button.
      return readKey(SETTINGS.frontButtonConfirm, fn);
    case Button::Left:
      // Logical Left maps to user-configured front button.
      return readKey(SETTINGS.frontButtonLeft, fn);
    case Button::Right:
      // Logical Right maps to user-configured front button.
      return readKey(SETTINGS.frontButtonRight, fn);
    case Button::Up:
      // Side buttons remain fixed for Up/Down.
      return readKey(HalGPIO::BTN_UP, fn);
    case Button::Down:
      // Side buttons remain fixed for Up/Down.
      return readKey(HalGPIO::BTN_DOWN, fn);
    case Button::Power:
      // Power button bypasses remapping.
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack:
      // Reader page navigation uses side buttons and can be swapped via settings.
      // On the X4 Pro the two side keys ARE the only physical navigation keys
      // (Left=GPIO0 wired to BTN_UP, Right=GPIO7 wired to BTN_DOWN), so the
      // PREV_NEXT default puts page-up on Left and page-down on Right. None of
      // this reads the touchscreen gate: the side keys keep turning pages with
      // the glass switched off, which is the whole point of being able to
      // switch it off. Pressed TOGETHER they are the control-centre chord
      // instead -- readKey() holds a single side press for one short window so
      // the two cannot be confused (see updateSideCombo).
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
          return readKey(isNavDirectionSwapped() ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP, fn);
        case CrossPointSettings::NEXT_PREV:
          return readKey(isNavDirectionSwapped() ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN, fn);
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
    case Button::PageForward:
      // Reader page navigation uses side buttons and can be swapped via settings.
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
          return readKey(isNavDirectionSwapped() ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN, fn);
        case CrossPointSettings::NEXT_PREV:
          return readKey(isNavDirectionSwapped() ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP, fn);
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
    case Button::NavNext:
      // Logical "next item" navigation: side Down + front Right, with the control axis flipped in
      // INVERTED / LANDSCAPE_CCW under the live orientation policy, matching the rotated hint labels.
      return isNavDirectionSwapped() ? (mapButton(Button::Up, fn) || mapButton(Button::Left, fn))
                                     : (mapButton(Button::Down, fn) || mapButton(Button::Right, fn));
    case Button::NavPrevious:
      // Logical "previous item" navigation: side Up + front Left, axis-flipped in the same orientations.
      return isNavDirectionSwapped() ? (mapButton(Button::Down, fn) || mapButton(Button::Right, fn))
                                     : (mapButton(Button::Up, fn) || mapButton(Button::Left, fn));
    case Button::ScreenLeft:
    case Button::ScreenRight:
    case Button::ScreenUp:
    case Button::ScreenDown:
      return mapButton(mapScreenDirection(button), fn);
  }

  return false;
}

namespace {
constexpr unsigned long TOUCH_DOWN_SELECT_DELAY_MS = 90;
constexpr unsigned long TOUCH_HELD_OVERRIDE_WINDOW_MS = 250;
}  // namespace

bool MappedInputManager::hasTouch() const { return gpio.hasTouch(); }

void MappedInputManager::rememberTouchHeldTime() const {
  touchHeldOverrideValid = true;
  touchHeldOverrideMs = gpio.lastTouchHeldMs();
  touchHeldOverrideAt = millis();
}

bool MappedInputManager::wasScreenTapped(int& x, int& y) const {
  if (!touchInputEnabled) return false;
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  rememberTouchHeldTime();
  return true;
}

bool MappedInputManager::wasScreenTouchDown(int& x, int& y) const {
  if (!touchInputEnabled) return false;
  float nx = 0.0f;
  float ny = 0.0f;
  unsigned long heldMs = 0;
  if (!gpio.isTouchTapCandidate(nx, ny, heldMs)) return false;
  if (heldMs < TOUCH_DOWN_SELECT_DELAY_MS) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenLongPress(int& x, int& y) const {
  if (!touchInputEnabled) return false;
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchLongPress(nx, ny)) return false;
  // Consuming the long-press implies acting on it: suppress the rest of the
  // contact so the finger lift can't also tap whatever the action opened.
  gpio.suppressTouchContact();
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::isScreenTouchHeld(int& x, int& y) const {
  if (!touchInputEnabled) return false;
  // Live contact position while the finger is down (no tap-slop gate) — drag tracking.
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.isTouchHeldAt(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenTouchReleased() const {
  return touchInputEnabled && gpio.wasTouchReleased();
}

bool MappedInputManager::wasBackButtonTap() const {
  // Only live while the current frame actually painted the chip: the flag is
  // cleared before every activity render and set by the theme that draws it,
  // so a screen without the chip (the reader page) can never route a stray
  // bottom-left tap into Back.
  if (!BaseTheme::touchBackButtonVisible()) return false;
  const Rect rect = BaseTheme::touchBackButtonHitRect(renderer);
  return wasTapInRect(rect.x, rect.y, rect.width, rect.height);
}

bool MappedInputManager::wasTapInRect(const int x, const int y, const int width, const int height) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) && tx >= x && tx < x + width && ty >= y && ty < y + height;
}

MappedInputManager::RowTouch MappedInputManager::rowTouch(int& row, const int top, const int rowStep,
                                                          const int rowCount, const int xStart, const int xEnd,
                                                          const int rowHeight) const {
  if (rowStep <= 0 || rowCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (x < xStart || x >= xEnd || y < top) return false;
    const int r = (y - top) / rowStep;
    if (r >= rowCount) return false;
    if (rowHeight > 0 && (y - top) % rowStep >= rowHeight) return false;
    row = r;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

MappedInputManager::RowTouch MappedInputManager::colTouch(int& col, const int left, const int colStep,
                                                          const int colCount, const int yStart, const int yEnd,
                                                          const int colWidth) const {
  if (colStep <= 0 || colCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (y < yStart || y >= yEnd || x < left) return false;
    const int c = (x - left) / colStep;
    if (c >= colCount) return false;
    if (colWidth > 0 && (x - left) % colStep >= colWidth) return false;
    col = c;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

bool MappedInputManager::decodeSwipe(int& sx, int& sy, int& ex, int& ey) const {
  if (!touchInputEnabled) return false;
  float nxs = 0.0f;
  float nys = 0.0f;
  float nxe = 0.0f;
  float nye = 0.0f;
  if (!gpio.wasSwipe(nxs, nys, nxe, nye)) return false;
  renderer.tapToLogical(nxs, nys, sx, sy);
  renderer.tapToLogical(nxe, nye, ex, ey);
  return true;
}

MappedInputManager::SwipeDir MappedInputManager::wasSwipe() const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return SwipeDir::None;
  switch (fui::swipeDirection(sx, sy, ex, ey)) {
    case fui::SwipeDir::Left:
      return SwipeDir::Left;
    case fui::SwipeDir::Right:
      return SwipeDir::Right;
    case fui::SwipeDir::Up:
      return SwipeDir::Up;
    case fui::SwipeDir::Down:
      return SwipeDir::Down;
    default:
      return SwipeDir::None;
  }
}

// Edge classification (which swipe counts as an edge gesture) lives in the
// SDK; only the MEANING of each edge — back, menu, home, light panel, and the
// home-key remap — is decided here.
bool MappedInputManager::wasEdgeSwipe(const freeink::ui::ScreenEdge edge) const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const bool hit = fui::edgeSwipe(edge, sx, sy, ex, ey, renderer.getScreenWidth(), renderer.getScreenHeight());
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasBackGesture() const {
  // Back = left-to-right swipe starting near the left edge. Edge-anchored so that
  // mid-screen horizontal swipes stay available to activities that consume
  // SwipeDir::Left/Right (e.g. percent selection, image viewer).
  return wasEdgeSwipe(fui::ScreenEdge::Left);
}

bool MappedInputManager::wasTopEdgeDownSwipe() const { return wasEdgeSwipe(fui::ScreenEdge::Top); }

bool MappedInputManager::wasBottomEdgeUpSwipe() const { return wasEdgeSwipe(fui::ScreenEdge::Bottom); }

bool MappedInputManager::wasMenuGesture() const { return wasTopEdgeDownSwipe(); }

bool MappedInputManager::wasReaderMenuSwipeUp() const { return gpio.hasHomeKey() && wasBottomEdgeUpSwipe(); }

bool MappedInputManager::wasHomeGesture() const {
  return gpio.hasHomeKey() ? gpio.wasHomeKeyTapped() : wasBottomEdgeUpSwipe();
}

bool MappedInputManager::wasHomeKeyHold() const {
  // Not gated on the touchscreen: the Home key is a separate signal on the touch
  // controller and stays live with the glass switched off, so its long-press
  // action keeps working there like the side keys do.
  return gpio.hasHomeKey() && gpio.wasHomeKeyLongPressed();
}

#if FREEINK_CAP_TOUCH
bool MappedInputManager::wasPowerShortClick() const {
  if (!gpio.hasTouch()) return false;
  // Gesture boards get the event from main.cpp's decoder, which is the only
  // place that knows whether the release was under the click ceiling. Other
  // touch boards can act on the release directly, bounded by the same ceiling.
  if (CrossPointSettings::usesPowerGestures()) return powerClickFrame;
  return gpio.wasReleased(HalGPIO::BTN_POWER) &&
         gpio.getPowerButtonHeldTime() <= CrossPointSettings::POWER_CLICK_MAX_HOLD_MS;
}

bool MappedInputManager::wasPowerConfirmClick() const {
  return SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PWR_CONFIRM && wasPowerShortClick();
}
#endif

bool MappedInputManager::wasPressed(const Button button) const {
  if (button == Button::Back && (wasBackGesture() || wasBackButtonTap())) return true;
#if FREEINK_CAP_TOUCH
  if (button == Button::Back && powerBackFrame) return true;
  if (button == Button::Confirm && wasPowerConfirmClick()) return true;
#endif
  return mapButton(button, &HalGPIO::wasPressed);
}

bool MappedInputManager::wasReleased(const Button button) const {
  if (button == Button::Back && (wasBackGesture() || wasBackButtonTap())) return true;
#if FREEINK_CAP_TOUCH
  if (button == Button::Back && powerBackFrame) return true;
  if (button == Button::Confirm && wasPowerConfirmClick()) return true;
  // On gesture boards the raw power release belongs to the decoder in main.cpp,
  // which republishes it as powerClickFrame once it knows the press really was a
  // click. Returning the raw edge here as well would let a Back hold also fire
  // whatever the short-click action happens to be.
  if (button == Button::Power && CrossPointSettings::usesPowerGestures()) return powerClickFrame;
#endif
  return mapButton(button, &HalGPIO::wasReleased);
}

bool MappedInputManager::wasLongPressed(const Button button, const unsigned long thresholdMs) const {
  if (!isPressed(button)) return false;
  const uint16_t bit = 1u << static_cast<uint8_t>(button);
  if ((longPressFiredButtons & bit) != 0 || getHeldTime() < thresholdMs) return false;
  longPressFiredButtons |= bit;
  suppressNextRelease(button);
  return true;
}

void MappedInputManager::suppressNextRelease(const Button button) const {
  suppressedReleaseButtons |= 1u << static_cast<uint8_t>(button);
}

bool MappedInputManager::consumeSuppressedRelease() const {
  uint16_t released = 0;
  for (uint8_t value = 0; value <= static_cast<uint8_t>(Button::ScreenDown); ++value) {
    const uint16_t bit = 1u << value;
    if ((suppressedReleaseButtons & bit) != 0 && mapButton(static_cast<Button>(value), &HalGPIO::wasReleased)) {
      released |= bit;
    }
  }
  suppressedReleaseButtons &= ~released;
  return released != 0;
}

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasAnyPressed() const { return gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const {
  if (!gpio.wasAnyPressed() && !gpio.wasAnyReleased() && touchHeldOverrideValid &&
      millis() - touchHeldOverrideAt <= TOUCH_HELD_OVERRIDE_WINDOW_MS) {
    return touchHeldOverrideMs;
  }
  touchHeldOverrideValid = false;
  return gpio.getHeldTime();
}

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Swap previous/next labels to match the page turn direction swap in INVERTED and LANDSCAPE_CCW.
  const bool swapLabels = isNavDirectionSwapped();
  const char* leftLabel = swapLabels ? next : previous;
  const char* rightLabel = swapLabels ? previous : next;

  return mapFrontLabels(back, confirm, leftLabel, rightLabel);
}

MappedInputManager::Labels MappedInputManager::mapDirectionalLabels(const char* back, const char* confirm,
                                                                    const char* left, const char* right, const char* up,
                                                                    const char* down) const {
  const auto labelForButton = [&](const Button rawButton) {
    if (mapScreenDirection(Button::ScreenLeft) == rawButton) return left;
    if (mapScreenDirection(Button::ScreenRight) == rawButton) return right;
    if (mapScreenDirection(Button::ScreenUp) == rawButton) return up;
    if (mapScreenDirection(Button::ScreenDown) == rawButton) return down;
    return "";
  };
  return mapFrontLabels(back, confirm, labelForButton(Button::Left), labelForButton(Button::Right));
}

MappedInputManager::Labels MappedInputManager::mapFrontLabels(const char* back, const char* confirm, const char* left,
                                                              const char* right) const {
  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    // Compare against configured logical roles and return the matching label.
    if (hw == SETTINGS.frontButtonBack) {
      return back;
    }
    if (hw == SETTINGS.frontButtonConfirm) {
      return confirm;
    }
    if (hw == SETTINGS.frontButtonLeft) {
      return left;
    }
    if (hw == SETTINGS.frontButtonRight) {
      return right;
    }
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}
