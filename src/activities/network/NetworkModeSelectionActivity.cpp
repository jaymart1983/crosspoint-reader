#include "NetworkModeSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
constexpr StrId menuItems[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
#if FREEINK_CAP_NETWORK
    StrId::STR_JOIN_NETWORK,
    StrId::STR_CALIBRE_WIRELESS,
    StrId::STR_CREATE_HOTSPOT,
#endif
#if FREEINK_CAP_USB_MSC
    StrId::STR_USB_DRIVE,
#endif
#if FREEINK_CAP_BLE_TRANSFER
    StrId::STR_BLUETOOTH_TRANSFER,
#endif
};
constexpr StrId menuDescs[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
#if FREEINK_CAP_NETWORK
    StrId::STR_JOIN_DESC,
    StrId::STR_CALIBRE_DESC,
    StrId::STR_HOTSPOT_DESC,
#endif
#if FREEINK_CAP_USB_MSC
    StrId::STR_USB_DRIVE_DESC,
#endif
#if FREEINK_CAP_BLE_TRANSFER
    StrId::STR_BLUETOOTH_TRANSFER_DESC,
#endif
};
constexpr UIIcon menuIcons[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
#if FREEINK_CAP_NETWORK
    UIIcon::Wifi,
    UIIcon::Library,
    UIIcon::Hotspot,
#endif
#if FREEINK_CAP_USB_MSC
    UIIcon::Usb,
#endif
#if FREEINK_CAP_BLE_TRANSFER
    UIIcon::Transfer,
#endif
};
constexpr NetworkMode menuModes[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
#if FREEINK_CAP_NETWORK
    NetworkMode::JOIN_NETWORK,
    NetworkMode::CONNECT_CALIBRE,
    NetworkMode::CREATE_HOTSPOT,
#endif
#if FREEINK_CAP_USB_MSC
    NetworkMode::USB_DRIVE,
#endif
#if FREEINK_CAP_BLE_TRANSFER
    NetworkMode::BLUETOOTH_TRANSFER,
#endif
};
}  // namespace

NetworkModeSelectionActivity::NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("NetworkModeSelection", renderer, mappedInput) {
  // Entirely static, so built once here rather than every buildScreen() call.
  for (int i = 0; i < MENU_ITEM_COUNT; i++) {
    fui::ListItem item;
    item.label = I18N.get(menuItems[i]);
    item.subtitle = I18N.get(menuDescs[i]);
    item.icon = listIconFor(menuIcons[i], 32);  // subtitle rows carry the larger icon
    item.actionValue = static_cast<int16_t>(i);
    rowItems_[i] = item;
  }
}

int NetworkModeSelectionActivity::listCount() const { return MENU_ITEM_COUNT; }

const char* NetworkModeSelectionActivity::headerTitle() const { return tr(STR_FILE_TRANSFER); }

void NetworkModeSelectionActivity::activateIndex(const int index) {
  // Selection leaves this screen; a lingering flash would gray an unrelated
  // element on the next render.
  app.clearTapFlash();
  nav.selected = index;

  onModeSelected(menuModes[index]);
}

void NetworkModeSelectionActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // rowItems_ was built once in the constructor and is reused here on every
  // repaint.
  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(MENU_ITEM_COUNT);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

void NetworkModeSelectionActivity::onModeSelected(NetworkMode mode) {
#if !FREEINK_CAP_NETWORK
  // No network stack means no CrossPointWebServerActivity to hand the choice
  // back to, so this screen is pushed as a top-level activity and dispatches
  // the two remaining transports itself.
#if FREEINK_CAP_USB_MSC
  if (mode == NetworkMode::USB_DRIVE) {
    activityManager.goToUsbDrive();
    return;
  }
#endif
#if FREEINK_CAP_BLE_TRANSFER
  if (mode == NetworkMode::BLUETOOTH_TRANSFER) {
    activityManager.goToBluetoothTransfer();
    return;
  }
#endif
#endif
  setResult(NetworkModeResult{mode});
  finish();
}

void NetworkModeSelectionActivity::onCancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
