#pragma once

#include <HalStorage.h>

#include "activities/Activity.h"
#include "components/UiAppHost.h"

class UsbDriveActivity final : public Activity, private UiAppHost {
 public:
  // `automatic` means nobody asked for this screen: a cable arrived and the
  // device mounted itself (see the plug-edge handler in main.cpp). The only
  // difference is how long it waits for a host before giving up -- a wall charger
  // is a plug edge too, and it is never going to enumerate.
  UsbDriveActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool automatic = false)
      : Activity("UsbDrive", renderer, mappedInput), UiAppHost(renderer), automatic(automatic) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::Connected || (!startFailed && state == State::IoError); }
  bool requiresExclusiveStorageLoop() const override { return true; }

 private:
  using State = UsbDriveState;

  // A user who chose this screen meant it, and may be walking to another room
  // for the cable. A device that mounted itself has no such excuse: if no host
  // has enumerated in twenty seconds it is a charger, and the reboot back to a
  // normal boot is cheap.
  static constexpr unsigned long HOST_WAIT_TIMEOUT_MS = 5UL * 60UL * 1000UL;
  static constexpr unsigned long AUTO_HOST_WAIT_TIMEOUT_MS = 20UL * 1000UL;
  static constexpr unsigned long START_FAILURE_TIMEOUT_MS = 30UL * 1000UL;
  static constexpr unsigned long FORCED_DISCONNECT_TIMEOUT_MS = 1000UL;

  static void driveScreen(UiScreen& screen, void* user);
  void buildDriveScreen(UiScreen& screen) const;
  void restartToHome();

  State state = State::Unsupported;
  const bool automatic;
  bool preparing = true;
  bool startFailed = false;
  bool restartRequested = false;
  bool forcedDisconnectRequested = false;
  unsigned long hostWaitStartedAt = 0;
  unsigned long startFailureStartedAt = 0;
  unsigned long forcedDisconnectRequestedAt = 0;
};
