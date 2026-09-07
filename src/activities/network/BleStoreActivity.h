#pragma once

#include <BoardConfig.h>

#if FREEINK_CAP_BLE_TRANSFER

#include <memory>
#include <string>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"  // Rect
#include "activities/network/BleStoreController.h"
#include "network/BleLink.h"

// The Store screen: a live browser over the phone app's Calibre library.
//
// All it owns is a BleStoreController and a screen. The radio, the auth gate and
// the transfer machinery are BleLink's and have been up since the device woke,
// which is what makes a store possible at all -- the old arrangement started the
// radio when this screen opened and stopped it when it closed, so the phone
// could never be reached from anywhere else and this screen could never inherit
// a link that was already alive.
//
// With no phone paired there is nothing to browse and nothing this screen can do
// about it, so it says so and offers the one control that helps: a button
// straight to Settings > Bluetooth.
class BleStoreActivity final : public Activity, public BleStoreController::Host, public BleLink::Observer {
 public:
  BleStoreActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // A catalogue page or a book on the wire must not be cut off by the inactivity
  // timer; an idle Store screen may sleep like any other.
  bool preventAutoSleep() override { return BLE_LINK.isBusy(); }

  // BleLink::Observer
  void onBleLinkChanged() override { requestUpdate(); }

  // BleStoreController::Host
  void storePublishStatus() override;
  void storeRepaint() override;
  void storeArmBookFetch(const std::string& filename) override;
  void storeFinish() override;
  void storeOpenBook(const std::string& path) override;

 private:
  std::unique_ptr<BleStoreController> store_;
  // Geometry of the "Pair a phone" tile, written by render() and read by the
  // touch hit-test. Zero height means it is not on screen.
  Rect pairRect_{0, 0, 0, 0};

  void renderPairPrompt();
  void openPairingPage();
};

#endif  // FREEINK_CAP_BLE_TRANSFER
