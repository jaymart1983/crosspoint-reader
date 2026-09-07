#pragma once

#include <BoardConfig.h>

#if FREEINK_CAP_BLE_TRANSFER

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"  // Rect
#include "network/BleLink.h"

// Settings > Bluetooth. The ONE place pairing happens.
//
// There used to be two: a Bluetooth Transfer screen that showed the code, and an
// error screen that replaced it the moment a trusted-host hello was refused --
// which is exactly the moment the code is the only way out. So this page has one
// invariant above all others: THE CODE IS ALWAYS ON IT. Paired or not,
// connected or not, whatever the last refusal was, the six digits are on the
// screen. A refusal is a small line underneath them, never a screen instead of
// them.
//
// The page owns no radio. BleLink has been advertising since the device woke, so
// this screen only reads it and repaints when it says something changed.
class BlePairingActivity final : public Activity, public BleLink::Observer {
 public:
  BlePairingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BlePairing", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // BleLink::Observer
  void onBleLinkChanged() override { requestUpdate(); }

 private:
  // Geometry of the Forget tile, written by render() and read by the touch
  // hit-test. Zero height means "not drawn", which is the unpaired case.
  Rect forgetRect_{0, 0, 0, 0};
  bool forgetSelected_ = false;

  void promptForget();
};

#endif  // FREEINK_CAP_BLE_TRANSFER
