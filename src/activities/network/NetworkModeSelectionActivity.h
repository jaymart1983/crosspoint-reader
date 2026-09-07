#pragma once

#include <BoardConfig.h>

#include "activities/UiListActivity.h"

enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT, USB_DRIVE, BLUETOOTH_TRANSFER };

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Connect to Calibre" - Use Calibre wireless device transfers
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 * - "Bluetooth Transfer" - Use BLE transfer with the browser companion or CLI
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 *
 * The header stays on GUI.drawHeader for the battery indicator.
 */
class NetworkModeSelectionActivity final : public UiListActivity {
 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  // Every row is capability-gated, so the count is computed rather than
  // hardcoded: Join / Calibre / Hotspot need the network stack, USB Drive needs
  // USB-MSC, Bluetooth Transfer needs BLE. On a FREEINK_CAP_NETWORK=0 board this
  // list is USB Drive + Bluetooth Transfer, and it is a top-level screen rather
  // than a sub-activity of the web server (see onModeSelected).
  static constexpr int MENU_ITEM_COUNT = 0
#if FREEINK_CAP_NETWORK
                                         + 3
#endif
#if FREEINK_CAP_USB_MSC
                                         + 1
#endif
#if FREEINK_CAP_BLE_TRANSFER
                                         + 1
#endif
      ;

  void onModeSelected(NetworkMode mode);
  void onCancel();

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override { onCancel(); }
  const char* headerTitle() const override;

  // Row storage: entirely static (label/subtitle/icon never change), so it's
  // built once in the constructor instead of every buildScreen() call, into
  // fixed-capacity storage that avoids any heap allocation for the row list.
  freeink::ui::ListItem rowItems_[MENU_ITEM_COUNT]{};
};
