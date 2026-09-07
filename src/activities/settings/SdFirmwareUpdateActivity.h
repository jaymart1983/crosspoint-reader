#pragma once

#include <string>

#include "activities/Activity.h"

/**
 * SD-card based firmware update activity.
 *
 * Flow:
 *  1) onEnter -> push FileBrowserActivity in PickFirmware mode (only .bin files visible).
 *  2) On result: validate the .bin (header magic, size fits OTA partition).
 *  3) Push ConfirmationActivity ("Update firmware?").
 *  4) On confirm: stream the file into the OTA partition via the Arduino Update API,
 *     drawing a progress bar; on success ESP.restart().
 *
 * Used from Settings -> System -> "SD Card Firmware Update", as the only activity
 * launched in boot recovery mode (left side button + power on X3), and -- with a
 * path already chosen -- for an image FirmwareWatcher found in the drop folder.
 * That last route skips step 1 only: the validation, the confirmation and the
 * flash are the same code, because the point of the drop folder was to remove a
 * second flashing path, not to add one.
 */
class SdFirmwareUpdateActivity : public Activity {
 public:
  enum class State {
    PICKING,
    VALIDATING,
    CONFIRMING,
    UPDATING,
    SUCCESS,
    FAILED,
  };

  explicit SdFirmwareUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool recoveryMode = false)
      : Activity("SdFirmwareUpdate", renderer, mappedInput), recoveryMode(recoveryMode) {}

  // Pre-chosen image: no picker, straight to validate-and-confirm. `stagedDrop`
  // says the file came out of the watched folder, which is the only case that
  // clears the folder afterwards -- an image the user picked by hand out of their
  // own directory is theirs, and deleting it would be a surprise.
  SdFirmwareUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path, bool stagedDrop)
      : Activity("SdFirmwareUpdate", renderer, mappedInput),
        firmwarePath(std::move(path)),
        presetPath(true),
        stagedDrop(stagedDrop) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::UPDATING || state == State::VALIDATING; }
  bool skipLoopDelay() override { return state == State::UPDATING; }

 private:
  State state = State::PICKING;
  bool recoveryMode = false;

  std::string firmwarePath;
  // The image was named by the caller rather than picked on screen, so a
  // cancelled confirmation leaves the screen instead of reopening a picker that
  // was never opened.
  bool presetPath = false;
  bool stagedDrop = false;
  size_t firmwareSize = 0;
  size_t writtenBytes = 0;
  unsigned int lastRenderedPercent = 101;
  std::string errorMessage;

  void launchPicker();
  void onPickerResult(const ActivityResult& result);
  bool validateFirmware();
  void promptConfirmation();
  void onConfirmationResult(const ActivityResult& result);
  void performUpdate();
};
