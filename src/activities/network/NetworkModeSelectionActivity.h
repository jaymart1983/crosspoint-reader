#pragma once

#include <BoardConfig.h>

// The WiFi transport chooser, and nothing else any more.
//
// It used to be the "File Transfer" menu: Join a Network, Calibre, Hotspot, USB
// Drive and Bluetooth Transfer, side by side. The last two are gone from it,
// because neither is something a user should have to start. USB Drive mounts
// itself when a cable is plugged into an awake device, and the BLE link is up
// for as long as the device is. What is left is the three WiFi modes, which
// genuinely are a choice, and which only exist on a board with a network stack --
// hence the guard around the whole file rather than around individual rows.
#if FREEINK_CAP_NETWORK

#include "activities/UiListActivity.h"

enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT, USB_DRIVE, BLUETOOTH_TRANSFER };

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Connect to Calibre" - Use Calibre wireless device transfers
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 *
 * The header stays on GUI.drawHeader for the battery indicator.
 *
 * NetworkMode keeps USB_DRIVE and BLUETOOTH_TRANSFER as enumerators: the value is
 * carried in an ActivityResult that CrossPointWebServerActivity switches on, and
 * removing enumerators from the middle would renumber the rest. No row produces
 * them.
 */
class NetworkModeSelectionActivity final : public UiListActivity {
 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int MENU_ITEM_COUNT = 3;

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

#endif  // FREEINK_CAP_NETWORK
