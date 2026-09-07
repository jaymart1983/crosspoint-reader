#include "BlePairingActivity.h"

#if FREEINK_CAP_BLE_TRANSFER

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <string>

#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

namespace {
constexpr int PAIRING_QR_SIZE = 160;
}  // namespace

void BlePairingActivity::onEnter() {
  Activity::onEnter();
  // The radio is already up. If it is not, something failed at boot and the page
  // should say so rather than silently showing a code nobody can use, so ask for
  // a start here too -- begin() is idempotent.
  BLE_LINK.begin();
  BLE_LINK.setObserver(this);
  forgetSelected_ = BLE_LINK.hasTrustedHost();
  requestUpdate();
}

void BlePairingActivity::onExit() {
  BLE_LINK.clearObserver(this);
  Activity::onExit();
}

void BlePairingActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (!BLE_LINK.hasTrustedHost()) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    promptForget();
    return;
  }

  int x = 0;
  int y = 0;
  if (forgetRect_.height > 0 && mappedInput.wasScreenTapped(x, y) && x >= forgetRect_.x &&
      x < forgetRect_.x + forgetRect_.width && y >= forgetRect_.y && y < forgetRect_.y + forgetRect_.height) {
    promptForget();
  }
}

void BlePairingActivity::promptForget() {
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_BLE_FORGET_HOST),
                                             BLE_LINK.trustedHostLabel()),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        {
          RenderLock lock(*this);
          if (!BLE_LINK.forgetTrustedHost()) LOG_ERR("BLE", "could not forget the trusted host");
        }
        forgetSelected_ = false;
        requestUpdate();
      });
}

void BlePairingActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int smallHeight = renderer.getLineHeight(SMALL_FONT_ID);

  const bool paired = BLE_LINK.hasTrustedHost();
  const std::string hostLabel = BLE_LINK.trustedHostLabel();
  const std::string& authError = BLE_LINK.authError();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLUETOOTH));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // --- who, if anyone -------------------------------------------------------
  if (paired) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_BLE_PAIRED_WITH));
    y += lineHeight;
    renderer.drawCenteredText(UI_12_FONT_ID, y, hostLabel.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID);
    renderer.drawCenteredText(SMALL_FONT_ID, y,
                              BLE_LINK.isAuthenticated() ? tr(STR_CONNECTED) : tr(STR_BLE_NOT_CONNECTED), true);
    y += smallHeight + metrics.verticalSpacing;
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_BLE_PAIR_PHONE), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID);
    renderer.drawCenteredText(SMALL_FONT_ID, y, tr(STR_BLE_PAIR_HINT), true);
    y += smallHeight + metrics.verticalSpacing;
  }

  // --- the code -------------------------------------------------------------
  // Drawn before anything below it is allowed to claim space, and never
  // conditional on any of it. See the class comment: every route out of a
  // pairing failure runs through these six digits.
  const std::string code = std::string(tr(STR_BLE_TRANSFER_CODE)) + BLE_LINK.sessionCode();
  renderer.drawCenteredText(UI_12_FONT_ID, y, code.c_str(), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;

  if (!authError.empty()) {
    // Under the code, in the small face: a refusal is a footnote to the code, not
    // a replacement for it.
    const std::string line = std::string(tr(STR_ERROR_MSG)) + ": " + authError;
    renderer.drawCenteredText(SMALL_FONT_ID, y, line.c_str(), true);
    y += smallHeight + metrics.verticalSpacing;
  }

  // --- the companion, and the room left for it ------------------------------
  // Everything below the QR is reserved before the QR is sized, so the QR is
  // what shrinks on a short panel or a tall theme -- never the code, and never
  // the Forget button.
  forgetRect_ = Rect{0, 0, 0, 0};
  const int forgetReserve = paired ? metrics.menuRowHeight + metrics.verticalSpacing : 0;
  const int reservedBelow = metrics.buttonHintsHeight + metrics.verticalSpacing + smallHeight + forgetReserve;
  const int available = pageHeight - y - reservedBelow;
  const int qrSize = std::min({PAIRING_QR_SIZE, pageWidth - metrics.contentSidePadding * 2, available});
  if (qrSize > 0) {
    QrUtils::drawQrCode(renderer, Rect{(pageWidth - qrSize) / 2, y, qrSize, qrSize}, BleLink::companionUrl());
    y += qrSize + metrics.verticalSpacing;
  }
  renderer.drawCenteredText(SMALL_FONT_ID, y, BleLink::companionUrl(), true);
  y += smallHeight + metrics.verticalSpacing;

  if (paired) {
    // One tile, drawn with the theme's own button-menu so it matches every other
    // button on the device. drawButtonMenu insets the tile by verticalSpacing.
    const Rect band{0, y, pageWidth, metrics.menuRowHeight + metrics.verticalSpacing};
    GUI.drawButtonMenu(
        renderer, band, 1, forgetSelected_ ? 0 : -1, [](int) { return std::string(tr(STR_FORGET_BUTTON)); },
        [](int) { return UIIcon::None; });
    forgetRect_ = Rect{metrics.contentSidePadding, y + metrics.verticalSpacing,
                       pageWidth - metrics.contentSidePadding * 2, metrics.menuRowHeight};
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), paired ? tr(STR_FORGET_BUTTON) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

#endif  // FREEINK_CAP_BLE_TRANSFER
