#pragma once

#include <HalStorage.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/sha256.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include "activities/Activity.h"
#include "activities/network/BleStoreController.h"

struct BleTransferRuntime;

// One BLE session, two faces.
//
// Mode::TRANSFER is the Bluetooth Transfer screen: pair, push books, pull the
// library. Mode::STORE is the Calibre store from the home screen. They share
// everything below the UI -- one NimBLE server, one hello/HMAC gate, one framed
// upload path with credit flow control and SHA-256, one download path -- because
// the store is a new *question*, not a new transport. What the store adds is the
// request channel: the device publishes what it wants in the `status`
// notification the app already subscribes to, and the app answers with an
// ordinary upload naming the same request id. See BleStoreController for why.
class BleTransferActivity final : public Activity, public BleStoreController::Host {
 public:
  enum class Mode { TRANSFER, STORE };

  enum class State {
    STARTING,
    ADVERTISING,
    CONNECTED,
    RECEIVING,
    VERIFYING,
    SAVED,
    FIRMWARE_CONFIRM,
    UPDATING,
    RESTARTING,
    PREPARING,
    SENDING,
    SENT,
    SAVE_HOST_PROMPT,
    FORGET_HOST_PROMPT,
    ERROR
  };
  enum class TransferKind {
    NONE,
    BOOK,
    BMP,
    FIRMWARE,
    PROGRESS,
    CRASH_REPORT,
    LIBRARY,
    PROGRESS_RESULT,
    CATALOG_PAGE,
    CATALOG_DETAIL
  };

  explicit BleTransferActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Mode mode = Mode::TRANSFER);
  ~BleTransferActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override {
    return state_ == State::VERIFYING || state_ == State::UPDATING || state_ == State::FIRMWARE_CONFIRM ||
           state_ == State::PREPARING || state_ == State::SENDING;
  }

  void enqueueBleConnected();
  void enqueueBleDisconnected();
  void enqueueControlWrite(const std::string& value);
  void enqueueDataWrite(const std::string& value);
  // Called from the NimBLE host task on connect and on every MTU exchange. Zero
  // means "nothing negotiated", which is read back as the 23-byte BLE default.
  void noteBleMtu(uint16_t mtu);
  // The most a notification may carry right now: ATT_MTU-3, never more than
  // BLE_STATUS_NOTIFY_MAX_BYTES. Public because the runtime sizes frames by it.
  size_t notifyCapBytes() const;

 private:
  friend struct BleTransferRuntime;
  enum class BleEventType { CONNECTED, DISCONNECTED, CONTROL, DATA };
  struct BleEvent {
    BleEventType type;
    std::string value;
  };

  const Mode mode_;
  // Present only in Mode::STORE. Owns the catalogue screens and the
  // request/timeout/retry state machine; owns no BLE of its own.
  std::unique_ptr<BleStoreController> store_;
  // The one filename the store has armed a `book` upload for. A book upload in
  // store mode that names anything else is refused: the device asked for a
  // specific book and must not accept a different one in its place.
  std::string storeExpectedBook_;

  State state_ = State::STARTING;
  std::unique_ptr<BleTransferRuntime> ble_;
  HalFile uploadFile_;
  HalFile downloadFile_;
  SemaphoreHandle_t eventMutex_ = nullptr;
  std::deque<BleEvent> bleEvents_;
  size_t queuedBleEventBytes_ = 0;
  bool bleEventOverflow_ = false;

  std::string sessionCode_;
  std::string fileName_;
  std::string partPath_;
  std::string finalPath_;
  std::string expectedSha256_;
  std::string savedPath_;
  std::string errorMessage_;
  std::string deviceId_;
  std::string deviceNonce_;
  std::string trustedHostName_;
  std::string candidateHostId_;
  std::string candidateHostName_;
  std::string candidateHostSecret_;

  TransferKind transferKind_ = TransferKind::NONE;
  State pendingFinalState_ = State::CONNECTED;
  size_t expectedSize_ = 0;
  size_t receivedBytes_ = 0;
  size_t sentBytes_ = 0;
  size_t flashWrittenBytes_ = 0;
  size_t lastProgressStatusBytes_ = 0;
  size_t lastDisplayProgressBytes_ = 0;
  unsigned int lastFirmwareFlashRenderedPercent_ = 101;
  // Outcome of the last `progress` batch. Deliberately not cleared by
  // resetTransfer(): the client reads the summary from the status published when
  // the batch finished, then issues a `start_get` for the per-entry document,
  // and that start_get resets the transfer state.
  uint32_t progressEntries_ = 0;
  uint32_t progressApplied_ = 0;
  size_t uploadChunkSize_ = 0;
  size_t uploadAckBytes_ = 0;
  size_t downloadChunkSize_ = 0;
  uint32_t expectedSequence_ = 0;
  uint32_t downloadSequence_ = 0;
  uint32_t pendingDownloadAck_ = 0;
  // Last MTU the peer negotiated, written from the NimBLE host task and read
  // from the activity loop. 0 until an exchange happens; see notifyCapBytes().
  std::atomic<uint16_t> negotiatedMtu_{0};
  bool helloAccepted_ = false;
  bool transferOpen_ = false;
  bool downloadOpen_ = false;
  bool downloadAwaitingAck_ = false;
  bool trustedHelloAccepted_ = false;
  bool hostPaired_ = false;
  bool hostPairSkipped_ = false;
  bool pendingCommit_ = false;
  bool statusDirty_ = true;
  bool removePartOnExit_ = false;
  bool uploadResumable_ = false;
  bool shaActive_ = false;
  mbedtls_sha256_context shaContext_;
  int promptSelection_ = 0;

  void enqueueBleEvent(BleEvent event);
  void processBleEvents();
  void onBleConnected();
  void onBleDisconnected();
  void onControlWrite(const std::string& value);
  void onDataWrite(const std::string& value);
  void processCommit();
  void startFileDownload(const char* path, const char* name, TransferKind kind, size_t offset, size_t chunkSize);
  void startCrashReportDownload(size_t offset, size_t chunkSize);
  void startLibraryDownload(size_t offset, size_t chunkSize);
  void startProgressResultDownload(size_t offset, size_t chunkSize);
  void processProgressBatch();
  void pumpDownload();
  void resetTransfer(bool removePart);
  void setState(State state);
  void setError(const std::string& error);
  // notifyStore=false refuses one request without failing the Store screen --
  // used for an answer to a question the device is no longer asking, which is a
  // normal race, not an error the user should see.
  void setError(const std::string& error, bool notifyStore);

  // BleStoreController::Host
  void storePublishStatus() override;
  void storeRepaint() override;
  void storeArmBookFetch(const std::string& filename) override;
  void storeFinish() override;
  void storeOpenBook(const std::string& path) override;
  void publishStatus();
  // READ is the authoritative document a GATT read returns -- everything the
  // session knows. NOTIFY is the doorbell: the same document with the fields a
  // client can re-read dropped, so it fits an ATT payload without truncation.
  enum class StatusScope { READ, NOTIFY };
  // `detail` narrows a NOTIFY document; it is ignored for READ. See
  // STATUS_DETAIL_MAX in the .cpp for what each level keeps.
  std::string buildStatusJson(StatusScope scope, unsigned detail) const;
  // The largest NOTIFY document that fits `capBytes`, shrinking a level at a
  // time. Never returns truncated JSON -- the floor is `{}`.
  std::string buildNotifyJson(size_t capBytes) const;
  bool setPendingTrustedHost(const std::string& hostId, const std::string& hostName, const std::string& secret);
  void completeFinalState(State finalState);
  void handleFirmwareConfirm();
  void handleSaveHostPrompt();
  void handleForgetHostPrompt();
  void renderCompanionReady(const std::string& primary, const std::string& secondary) const;
  void renderFirmwareConfirm() const;
  void renderFirmwareUpdating() const;
  void renderSaveHostPrompt() const;
  void renderForgetHostPrompt() const;
};
