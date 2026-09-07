#include "BleStoreActivity.h"

#if FREEINK_CAP_BLE_TRANSFER

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/settings/BlePairingActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

BleStoreActivity::BleStoreActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    // ActivityManager::goHome() reads this name to decide which home row to land
    // back on.
    : Activity("Store", renderer, mappedInput) {
  store_ = makeUniqueNoThrow<BleStoreController>(renderer, mappedInput, *this);
  if (!store_) LOG_ERR("STORE", "OOM: store controller");
}

void BleStoreActivity::onEnter() {
  Activity::onEnter();
  // The radio has been up since the device woke; this only borrows it.
  BLE_LINK.begin();
  BLE_LINK.setObserver(this);
  if (store_) {
    store_->begin();
    // attachStore() replays "the app is already through the gate" when it is, so
    // a Store opened onto a live session asks for page one immediately instead of
    // waiting for a reconnect that is not coming.
    BLE_LINK.attachStore(store_.get());
  }
  requestUpdate();
}

void BleStoreActivity::onExit() {
  BLE_LINK.clearObserver(this);
  if (store_) {
    BLE_LINK.detachStore(store_.get());
    // The catalogue and its thumbnails are this screen's, not the link's: the
    // Store is live or it is nothing, so nothing survives the screen closing.
    store_->end();
  }
  Activity::onExit();
}

void BleStoreActivity::loop() {
  if (!store_) {
    finish();
    return;
  }

  if (!BLE_LINK.hasTrustedHost()) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      openPairingPage();
      return;
    }
    int x = 0;
    int y = 0;
    if (pairRect_.height > 0 && mappedInput.wasScreenTapped(x, y) && x >= pairRect_.x &&
        x < pairRect_.x + pairRect_.width && y >= pairRect_.y && y < pairRect_.y + pairRect_.height) {
      openPairingPage();
    }
    return;
  }

  // Deadlines, republishes and the transfer pump belong to the link and are
  // driven from the main loop whatever is on screen. Only input is this
  // screen's.
  store_->handleInput();
}

void BleStoreActivity::openPairingPage() {
  auto activity = makeUniqueNoThrow<BlePairingActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("STORE", "OOM: pairing activity");
    return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult&) {
    // The pairing page took the observer slot on the way in (there is only one --
    // only the frontmost screen has anything to repaint) and cleared it on the way
    // out. Take it back, or this screen would sit there not noticing the phone it
    // was just paired with connecting.
    BLE_LINK.setObserver(this);
    requestUpdate();
  });
}

void BleStoreActivity::render(RenderLock&&) {
  if (!BLE_LINK.hasTrustedHost() || !store_) {
    renderPairPrompt();
    return;
  }
  store_->render();
}

void BleStoreActivity::renderPairPrompt() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_STORE));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 3;
  renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_STORE_NEEDS_PAIRING), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;
  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_STORE_NEEDS_PAIRING_HINT), true);
  y += lineHeight + metrics.verticalSpacing * 2;

  // The one control that helps. No code and no QR here: pairing happens on one
  // page and this is not it, so the button goes there rather than growing a
  // second half-copy of it.
  const Rect band{0, y, pageWidth, metrics.menuRowHeight + metrics.verticalSpacing};
  GUI.drawButtonMenu(
      renderer, band, 1, 0, [](int) { return std::string(tr(STR_STORE_PAIR_ACTION)); },
      [](int) { return UIIcon::Transfer; });
  pairRect_ = Rect{metrics.contentSidePadding, y + metrics.verticalSpacing,
                   pageWidth - metrics.contentSidePadding * 2, metrics.menuRowHeight};

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void BleStoreActivity::storePublishStatus() {
  BLE_LINK.publishStatusNow();
  requestUpdate();
}

void BleStoreActivity::storeRepaint() { requestUpdate(); }

void BleStoreActivity::storeArmBookFetch(const std::string& filename) { BLE_LINK.armStoreBookFetch(filename); }

void BleStoreActivity::storeFinish() { finish(); }

void BleStoreActivity::storeOpenBook(const std::string& path) { activityManager.goToReader(path); }

#endif  // FREEINK_CAP_BLE_TRANSFER
