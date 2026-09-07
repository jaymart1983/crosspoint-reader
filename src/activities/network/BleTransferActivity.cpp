#include "BleTransferActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <HalClock.h>
#include <NimBLEDevice.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <mbedtls/md.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <utility>

#include "BleTrustedHostStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/FirmwareFlasher.h"
#include "util/BleCatalog.h"
#include "util/BookCacheUtils.h"
#include "util/BookLibraryIndex.h"
#include "util/BookProgressSync.h"
#include "util/QrUtils.h"
#include "util/TaskWatchdog.h"

namespace {

constexpr const char* BLE_DEVICE_NAME = "CrossPoint Transfer";
constexpr const char* BLE_SERVICE_UUID = "6f9f0a00-9b1d-4d1f-9f53-5b6b8b3d0f10";
constexpr const char* BLE_CONTROL_UUID = "6f9f0a01-9b1d-4d1f-9f53-5b6b8b3d0f10";
constexpr const char* BLE_DATA_IN_UUID = "6f9f0a02-9b1d-4d1f-9f53-5b6b8b3d0f10";
constexpr const char* BLE_STATUS_UUID = "6f9f0a03-9b1d-4d1f-9f53-5b6b8b3d0f10";
constexpr const char* BLE_DATA_OUT_UUID = "6f9f0a04-9b1d-4d1f-9f53-5b6b8b3d0f10";
constexpr const char* BLE_TRANSFER_WEB_URL = "https://ble.xteink.lol/";
constexpr const char* BOOKS_ROOT = "/Books";
constexpr const char* PICTURES_ROOT = "/Pictures";
constexpr const char* BLE_OTA_ROOT = "/.crosspoint/ble-ota";
constexpr const char* BLE_OTA_PART_PATH = "/.crosspoint/ble-ota/firmware.bin.part";
constexpr const char* BLE_OTA_FINAL_PATH = "/.crosspoint/ble-ota/firmware.bin";
constexpr const char* CRASH_REPORT_PATH = "/crash_report.txt";
constexpr const char* CRASH_REPORT_NAME = "crash_report.txt";
// The library listing is staged on SD rather than held in RAM, then served
// through the same frame/ack path as any other download -- which is also what
// gives it a known size and a resumable offset.
constexpr const char* LIBRARY_INDEX_PATH = "/.crosspoint/ble-library.json";
constexpr const char* LIBRARY_INDEX_NAME = "library.json";
constexpr const char* CROSSPOINT_ROOT = "/.crosspoint";
// A `progress` batch is staged like any other upload -- part file, SHA-256 over
// the whole thing, rename on commit -- and only then parsed. Verifying before
// touching a single book means a truncated batch cannot half-apply.
constexpr const char* PROGRESS_BATCH_PART_PATH = "/.crosspoint/ble-progress.json.part";
constexpr const char* PROGRESS_BATCH_PATH = "/.crosspoint/ble-progress.json";
constexpr const char* PROGRESS_BATCH_NAME = "progress.json";
// Per-entry outcomes go to SD and are served as an ordinary download. They do
// not fit in `status`: a notification carries at most ATT_MTU-3 bytes, so a
// shelf-sized result array would be silently truncated on the wire.
constexpr const char* PROGRESS_RESULT_PATH = "/.crosspoint/ble-progress-result.json";
constexpr const char* PROGRESS_RESULT_NAME = "progress-result.json";
// A catalogue page or book detail is staged exactly like every other upload --
// part file, SHA-256 over the whole thing, rename on commit -- and only then
// unpacked. The store's own scratch lives under /.crosspoint/store so closing
// the screen can clear the whole lot in one place.
constexpr const char* STORE_ROOT = "/.crosspoint/store";
constexpr const char* CATALOG_PART_PATH = "/.crosspoint/store/catalog.bin.part";
constexpr const char* CATALOG_PATH = "/.crosspoint/store/catalog.bin";
constexpr const char* CATALOG_NAME = "catalog.bin";
constexpr size_t MIN_BLE_FIRMWARE_BYTES = 64UL * 1024UL;
constexpr size_t MAX_BLE_BOOK_BYTES = 32UL * 1024UL * 1024UL;
constexpr size_t MAX_BLE_BMP_BYTES = 8UL * 1024UL * 1024UL;
// ~120 bytes per entry, so this is a shelf of a few thousand books with room to
// spare, and still a bounded amount of SD scratch.
constexpr size_t MAX_BLE_PROGRESS_BYTES = 512UL * 1024UL;
// One entry is parsed at a time and never exceeds this; the cap is what keeps a
// hostile or corrupt document from growing a std::string without bound.
constexpr size_t MAX_PROGRESS_ENTRY_BYTES = 640;
constexpr uint32_t MAX_PROGRESS_ENTRIES = 8192;
// A book path relative to /Books. Longer than MAX_FILENAME_BYTES because the
// `library` listing emits sub-folder paths and this must round-trip them.
constexpr size_t MAX_BOOK_PATH_BYTES = 255;
constexpr size_t BLE_DOWNLOAD_CHUNK_BYTES = 160;
constexpr size_t BLE_DOWNLOAD_CHUNK_BYTES_MIN = 20;
constexpr size_t BLE_DOWNLOAD_CHUNK_BYTES_MAX = BLE_DOWNLOAD_CHUNK_BYTES;
constexpr size_t BLE_RESUME_HASH_CHUNK_BYTES = 512;
constexpr size_t MAX_FILENAME_BYTES = 96;
constexpr size_t BLE_HOST_ID_MAX_BYTES = 64;
constexpr size_t BLE_HOST_NAME_MAX_BYTES = 48;
constexpr size_t BLE_SHARED_SECRET_HEX_BYTES = 64;
constexpr size_t BLE_NONCE_BYTES = 16;
constexpr size_t BLE_PROGRESS_STATUS_INTERVAL_BYTES = 4UL * 1024UL;
constexpr size_t BLE_PROGRESS_DISPLAY_INTERVAL_BYTES = 128UL * 1024UL;
constexpr size_t BLE_FIRMWARE_PROGRESS_DISPLAY_INTERVAL_BYTES = 1024UL * 1024UL;
constexpr size_t BLE_UPLOAD_ACK_BYTES_MIN = 20;
constexpr size_t BLE_UPLOAD_ACK_BYTES_MAX = 64UL * 1024UL;
constexpr size_t MAX_QUEUED_BLE_EVENTS = 64;
constexpr size_t MAX_QUEUED_BLE_EVENT_BYTES = 8UL * 1024UL;
constexpr size_t EPUB_SUFFIX_LEN = 5;
constexpr size_t BMP_SUFFIX_LEN = 4;
constexpr size_t BIN_SUFFIX_LEN = 4;
constexpr int BLE_TRANSFER_QR_SIZE = 172;
// A GATT notification carries at most ATT_MTU-3 bytes, and the peer decides the
// MTU. Until it has exchanged one the only defensible assumption is the 23-byte
// BLE minimum -- 20 bytes of payload.
constexpr uint16_t BLE_ATT_MTU_MINIMUM = 23;
constexpr size_t BLE_ATT_NOTIFY_OVERHEAD = 3;
// Even when the peer grants 517 the status notification stays inside this. It is
// ATT_MTU-3 for the ~185-byte MTU that iOS and most Android stacks settle on, so
// the doorbell survives a re-negotiation downwards, a stack that reports the MTU
// it asked for rather than the one in force, and a reconnect that never
// exchanges at all. The whole document is on the READ; there is nothing to gain
// by filling 514 bytes of notification with it.
constexpr size_t BLE_STATUS_NOTIFY_MAX_BYTES = 180;
// Shrink levels for a NOTIFY document, richest first. Each level drops the next
// least useful group of fields; level 0 is `{"state":"..."}` alone, and the
// floor below that is the empty object. Nothing is ever cut mid-string.
//   5  everything a notification may carry
//   4  - protocol_version, store_supported, clock_supported, device_time
//   3  - trusted_host, paired, pairing, mode, name, path
//   2  - the pending block shrinks to the `req`/`op` an answer must quote back
//   1  - the transfer counters and the error text
//   0  - the pending block
// The two things a live session cannot lose sit at the bottom of the order on
// purpose. `received` IS the credit ack an upload waits on (see onDataWrite), so
// a notification that drops it stalls the transfer. The `pending` geometry is
// what the app builds its answer from, so it stays whole down to level 3 -- at
// 180 bytes every real store request still fits there, and only a book arriving
// while a fetch is outstanding pushes as far as level 2.
constexpr unsigned STATUS_DETAIL_MAX = 5;
// The floor of the ladder must itself be sendable on the worst link there is,
// or "never truncate" is a promise the code cannot keep.
static_assert(sizeof("{}") - 1 <= BLE_ATT_MTU_MINIMUM - BLE_ATT_NOTIFY_OVERHEAD,
              "the empty-object fallback must fit a 23-byte ATT MTU");
static_assert(BLE_STATUS_NOTIFY_MAX_BYTES <= 517 - BLE_ATT_NOTIFY_OVERHEAD,
              "the notify cap must fit the largest MTU this server asks for");

std::string makeSessionCode() {
  char buffer[7];
  snprintf(buffer, sizeof(buffer), "%06u", static_cast<unsigned>(esp_random() % 1000000UL));
  return buffer;
}

std::string bytesToHex(const uint8_t* data, const size_t length) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.resize(length * 2);
  for (size_t i = 0; i < length; i++) {
    out[i * 2] = hex[data[i] >> 4];
    out[i * 2 + 1] = hex[data[i] & 0x0F];
  }
  return out;
}

std::string makeDeviceId() {
  uint8_t mac[6] = {};
  esp_efuse_mac_get_default(mac);
  return bytesToHex(mac, sizeof(mac));
}

std::string makeNonceHex() {
  uint8_t nonce[BLE_NONCE_BYTES] = {};
  for (auto& byte : nonce) byte = static_cast<uint8_t>(esp_random() & 0xFF);
  return bytesToHex(nonce, sizeof(nonce));
}

std::string toLowerAscii(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

bool isHexString(const std::string& value, const size_t length) {
  if (value.length() != length) return false;
  return std::all_of(value.begin(), value.end(), [](const char c) {
    return std::isdigit(static_cast<unsigned char>(c)) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  });
}

bool isHexSha256(const std::string& value) { return isHexString(value, 64); }

bool endsWithSuffix(const std::string& value, const char* suffix, const size_t suffixLen) {
  if (value.length() < suffixLen) return false;
  return toLowerAscii(value.substr(value.length() - suffixLen)) == suffix;
}

bool isSafeBleFileName(const std::string& value) {
  if (value.empty() || value.length() > MAX_FILENAME_BYTES || value[0] == '.') return false;
  for (const char c : value) {
    const auto uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) || c == '.' || c == '_' || c == '-') continue;
    return false;
  }
  return true;
}

bool isSafeHostId(const std::string& value) {
  if (value.empty() || value.length() > BLE_HOST_ID_MAX_BYTES) return false;
  return std::all_of(value.begin(), value.end(), [](const char c) {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '-' || c == '_';
  });
}

std::string sanitizeHostName(std::string value) {
  if (value.empty()) return "Trusted host";
  if (value.length() > BLE_HOST_NAME_MAX_BYTES) value.resize(BLE_HOST_NAME_MAX_BYTES);
  for (char& c : value) {
    const auto uc = static_cast<unsigned char>(c);
    if (uc < 32 || uc > 126) c = '?';
  }
  return value;
}

bool isSafeBleBookName(const std::string& value) {
  return isSafeBleFileName(value) && endsWithSuffix(value, ".epub", EPUB_SUFFIX_LEN);
}

bool isSafeBleBmpName(const std::string& value) {
  return isSafeBleFileName(value) && endsWithSuffix(value, ".bmp", BMP_SUFFIX_LEN);
}

bool isSafeBleFirmwareName(const std::string& value) {
  return isSafeBleFileName(value) && endsWithSuffix(value, ".bin", BIN_SUFFIX_LEN);
}

// A path relative to the books root, as the `library` listing emits it
// ("Sub/Folder/Book.epub"). Deliberately more permissive than
// isSafeBleFileName(): that guards a name the client invents for a new file,
// whereas this must accept every name already on the card, including UTF-8 and
// the punctuation real book titles carry. What it does not accept is anything
// that could leave /Books or name a hidden entry.
bool isSafeBleBookRelativePath(const std::string& value) {
  if (value.empty() || value.length() > MAX_BOOK_PATH_BYTES) return false;
  size_t segmentStart = 0;
  for (size_t i = 0; i <= value.size(); i++) {
    if (i < value.size()) {
      const auto uc = static_cast<unsigned char>(value[i]);
      if (uc < 0x20 || uc == 0x7F) return false;  // control characters
      if (value[i] == '\\') return false;         // never a separator here; confuses hosts
      if (value[i] != '/') continue;
    }
    // An empty segment is a leading '/', a trailing '/', or "//".
    if (i == segmentStart) return false;
    // Rejects "." and ".." -- which would walk out of the books root -- along
    // with the reader's own dot-caches and host-OS litter, none of which the
    // listing ever emits.
    if (value[segmentStart] == '.') return false;
    segmentStart = i + 1;
  }
  return true;
}

// Pulls one top-level object at a time out of a JSON array held in a file.
//
// The batch is parsed incrementally on purpose: a few thousand entries is
// hundreds of kilobytes, and a reading session has no such heap to spare. Only
// the current object's text is in RAM, capped at MAX_PROGRESS_ENTRY_BYTES, and
// ArduinoJson is handed that one object rather than the document.
//
// This is a brace matcher, not a JSON parser -- it only needs to find where each
// object ends, which means tracking strings and their escapes so a '}' inside a
// filename does not end the object early. Everything inside is then parsed
// properly by ArduinoJson, which is what rejects malformed entries.
class ProgressBatchReader {
 public:
  enum class Next { OBJECT, END, PARSE_ERROR };

  explicit ProgressBatchReader(HalFile& file) : file_(file) {}

  Next next(std::string& objectText) {
    objectText.clear();
    if (!sawArrayStart_) {
      if (skipWhitespace() != '[') return Next::PARSE_ERROR;
      sawArrayStart_ = true;
    }
    int c = skipWhitespace();
    if (c == ']') return Next::END;
    if (!firstEntry_) {
      if (c != ',') return Next::PARSE_ERROR;
      c = skipWhitespace();
    }
    firstEntry_ = false;
    if (c != '{') return Next::PARSE_ERROR;

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    while (true) {
      if (objectText.size() >= MAX_PROGRESS_ENTRY_BYTES) return Next::PARSE_ERROR;
      objectText.push_back(static_cast<char>(c));
      if (inString) {
        if (escaped) {
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if (c == '"') {
          inString = false;
        }
      } else if (c == '"') {
        inString = true;
      } else if (c == '{' || c == '[') {
        depth++;
      } else if (c == '}' || c == ']') {
        depth--;
        if (depth == 0) return Next::OBJECT;
      }
      c = readByte();
      if (c < 0) return Next::PARSE_ERROR;
    }
  }

 private:
  // Buffered: reading a few hundred entries one SD call per byte would be
  // minutes of SPI overhead.
  int readByte() {
    if (bufferPos_ >= bufferLen_) {
      const int read = file_.read(buffer_.data(), buffer_.size());
      if (read <= 0) return -1;
      bufferLen_ = static_cast<size_t>(read);
      bufferPos_ = 0;
    }
    return buffer_[bufferPos_++];
  }

  int skipWhitespace() {
    while (true) {
      const int c = readByte();
      if (c < 0) return -1;
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
      return c;
    }
  }

  HalFile& file_;
  std::array<uint8_t, 256> buffer_ = {};
  size_t bufferLen_ = 0;
  size_t bufferPos_ = 0;
  bool sawArrayStart_ = false;
  bool firstEntry_ = true;
};

std::string transferKindName(const BleTransferActivity::TransferKind kind) {
  switch (kind) {
    case BleTransferActivity::TransferKind::BOOK:
      return "book";
    case BleTransferActivity::TransferKind::BMP:
      return "bmp";
    case BleTransferActivity::TransferKind::FIRMWARE:
      return "firmware";
    case BleTransferActivity::TransferKind::PROGRESS:
      return "progress";
    case BleTransferActivity::TransferKind::PROGRESS_RESULT:
      return "progress_result";
    case BleTransferActivity::TransferKind::CRASH_REPORT:
      return "crash_report";
    case BleTransferActivity::TransferKind::LIBRARY:
      return "library";
    case BleTransferActivity::TransferKind::CATALOG_PAGE:
      return "catalog_page";
    case BleTransferActivity::TransferKind::CATALOG_DETAIL:
      return "catalog_detail";
    case BleTransferActivity::TransferKind::NONE:
      return "";
  }
  return "";
}

std::string sha256ToHex(const uint8_t digest[32]) { return bytesToHex(digest, 32); }

std::string trustedHostMessage(const std::string& nonce, const std::string& hostId) {
  return nonce + "|" + hostId + "|1";
}

std::string hmacSha256Hex(const std::string& secret, const std::string& message) {
  uint8_t output[32] = {};
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md) return "";
  const int ret = mbedtls_md_hmac(md, reinterpret_cast<const uint8_t*>(secret.data()), secret.size(),
                                  reinterpret_cast<const uint8_t*>(message.data()), message.size(), output);
  if (ret != 0) return "";
  return bytesToHex(output, sizeof(output));
}

std::string stateName(BleTransferActivity::State state) {
  switch (state) {
    case BleTransferActivity::State::STARTING:
      return "starting";
    case BleTransferActivity::State::ADVERTISING:
      return "advertising";
    case BleTransferActivity::State::CONNECTED:
      return "connected";
    case BleTransferActivity::State::RECEIVING:
      return "receiving";
    case BleTransferActivity::State::VERIFYING:
      return "verifying";
    case BleTransferActivity::State::SAVED:
      return "saved";
    case BleTransferActivity::State::FIRMWARE_CONFIRM:
      return "confirming";
    case BleTransferActivity::State::UPDATING:
      return "updating";
    case BleTransferActivity::State::RESTARTING:
      return "restarting";
    case BleTransferActivity::State::PREPARING:
      return "preparing";
    case BleTransferActivity::State::SENDING:
      return "sending";
    case BleTransferActivity::State::SENT:
      return "sent";
    case BleTransferActivity::State::SAVE_HOST_PROMPT:
      return "save_host_prompt";
    case BleTransferActivity::State::FORGET_HOST_PROMPT:
      return "forget_host_prompt";
    case BleTransferActivity::State::ERROR:
      return "error";
  }
  return "unknown";
}

uint32_t readLe32(const std::string& value) {
  assert(value.size() >= sizeof(uint32_t));
  const auto* b = reinterpret_cast<const uint8_t*>(value.data());
  return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
         (static_cast<uint32_t>(b[3]) << 24);
}

bool constantTimeEquals(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < left.size(); i++) {
    diff |= static_cast<uint8_t>(left[i]) ^ static_cast<uint8_t>(right[i]);
  }
  return diff == 0;
}

bool hashExistingPrefix(const std::string& path, size_t bytes, mbedtls_sha256_context& context) {
  HalFile file;
  if (!Storage.openFileForRead("BLE", path, file)) return false;

  std::array<uint8_t, BLE_RESUME_HASH_CHUNK_BYTES> buffer = {};
  while (bytes > 0) {
    const size_t wanted = std::min(bytes, buffer.size());
    const int read = file.read(buffer.data(), wanted);
    if (read <= 0) {
      file.close();
      return false;
    }
    mbedtls_sha256_update(&context, buffer.data(), static_cast<size_t>(read));
    bytes -= static_cast<size_t>(read);
  }

  file.close();
  return true;
}

class ServerCallbacks final : public NimBLEServerCallbacks {
 public:
  explicit ServerCallbacks(BleTransferActivity& activity) : activity_(activity) {}

  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    server->updateConnParams(connInfo.getConnHandle(), 6, 12, 0, 120);
    server->setDataLen(connInfo.getConnHandle(), 251);
    // Still the 23-byte default at this point on most stacks; onMTUChange
    // corrects it a moment later. Recorded either way so a peer that never
    // exchanges is sized for honestly rather than optimistically.
    activity_.noteBleMtu(connInfo.getMTU());
    activity_.enqueueBleConnected();
  }

  void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override { activity_.noteBleMtu(mtu); }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    activity_.noteBleMtu(0);
    activity_.enqueueBleDisconnected();
  }

 private:
  BleTransferActivity& activity_;
};

class ControlCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit ControlCallbacks(BleTransferActivity& activity) : activity_(activity) {}

  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    activity_.enqueueControlWrite(characteristic->getValue());
  }

 private:
  BleTransferActivity& activity_;
};

class DataCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit DataCallbacks(BleTransferActivity& activity) : activity_(activity) {}

  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    activity_.enqueueDataWrite(characteristic->getValue());
  }

 private:
  BleTransferActivity& activity_;
};

}  // namespace

struct BleTransferRuntime {
  explicit BleTransferRuntime(BleTransferActivity& owner)
      : activity(owner), serverCallbacks(owner), controlCallbacks(owner), dataCallbacks(owner) {}

  BleTransferActivity& activity;
  NimBLEServer* server = nullptr;
  NimBLEService* service = nullptr;
  NimBLECharacteristic* status = nullptr;
  NimBLECharacteristic* dataOut = nullptr;
  ServerCallbacks serverCallbacks;
  ControlCallbacks controlCallbacks;
  DataCallbacks dataCallbacks;

  bool begin() {
    NimBLEDevice::init(BLE_DEVICE_NAME);
    NimBLEDevice::setMTU(517);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    server = NimBLEDevice::createServer();
    if (!server) return false;
    server->setCallbacks(&serverCallbacks, false);

    service = server->createService(BLE_SERVICE_UUID);
    if (!service) return false;

    auto* control = service->createCharacteristic(BLE_CONTROL_UUID, NIMBLE_PROPERTY::WRITE);
    auto* dataIn = service->createCharacteristic(BLE_DATA_IN_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    status = service->createCharacteristic(BLE_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    dataOut = service->createCharacteristic(BLE_DATA_OUT_UUID, NIMBLE_PROPERTY::NOTIFY);
    if (!control || !dataIn || !status || !dataOut) return false;

    control->setCallbacks(&controlCallbacks);
    dataIn->setCallbacks(&dataCallbacks);
    // The stored value is the authoritative document from the first moment: a
    // client that reads before it ever sees a notification still gets the whole
    // truth.
    status->setValue(activity.buildStatusJson(BleTransferActivity::StatusScope::READ, STATUS_DETAIL_MAX));

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(BLE_SERVICE_UUID);
    advertising->setName(BLE_DEVICE_NAME);
    advertising->start();
    return true;
  }

  void publish(const std::string& readJson, const std::string& notifyJson) {
    if (!status) return;
    const size_t notifyCap = activity.notifyCapBytes();
    const uint16_t mtu = activity.negotiatedMtu_.load(std::memory_order_relaxed);
    // Two different payloads on one characteristic. setValue() is what a GATT
    // read returns; notify(buffer, length) sends *that* buffer instead of the
    // stored value, so the doorbell can be small while the read stays whole.
    // Confirmed present in the pinned NimBLE-Arduino:
    //   bool notify(const uint8_t* value, size_t length, uint16_t connHandle) const
    status->setValue(readJson);
    LOG_DBG("BLE", "status: notify %u bytes, read %u bytes, cap %u (mtu %u)",
            static_cast<unsigned>(notifyJson.size()), static_cast<unsigned>(readJson.size()),
            static_cast<unsigned>(notifyCap), static_cast<unsigned>(mtu));
    status->notify(reinterpret_cast<const uint8_t*>(notifyJson.data()), notifyJson.size());
  }

  void notifyData(const uint8_t* data, const size_t length) {
    if (!dataOut) return;
    const size_t notifyCap = activity.notifyCapBytes();
    // The client picks the chunk size and its resume arithmetic depends on it,
    // so this is never silently shrunk -- but a frame the link cannot carry is
    // the same class of bug as an overlong status, and must not be silent.
    if (length > notifyCap) {
      LOG_DBG("BLE", "data frame %u bytes exceeds notify cap %u", static_cast<unsigned>(length),
              static_cast<unsigned>(notifyCap));
    }
    dataOut->setValue(data, length);
    dataOut->notify();
  }

  void startAdvertising() {
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (advertising) advertising->start();
  }

  void end() {
    NimBLEDevice::stopAdvertising();
    NimBLEDevice::deinit(true);
    server = nullptr;
    service = nullptr;
    status = nullptr;
    dataOut = nullptr;
  }
};

BleTransferActivity::BleTransferActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const Mode mode)
    // The name is what ActivityManager::goHome() reads to decide which home row
    // to land on, so the two modes must not share one.
    : Activity(mode == Mode::STORE ? "Store" : "BleTransfer", renderer, mappedInput),
      mode_(mode),
      eventMutex_(xSemaphoreCreateMutex()) {
  if (mode_ == Mode::STORE) {
    store_ = makeUniqueNoThrow<BleStoreController>(renderer, mappedInput, *this);
    if (!store_) LOG_ERR("BLE", "OOM: store controller");
  }
}

BleTransferActivity::~BleTransferActivity() {
  if (eventMutex_) {
    vSemaphoreDelete(eventMutex_);
    eventMutex_ = nullptr;
  }
}

void BleTransferActivity::onEnter() {
  Activity::onEnter();
  sessionCode_ = makeSessionCode();
  deviceId_ = makeDeviceId();
  deviceNonce_ = makeNonceHex();
  {
    RenderLock lock(*this);
    BLE_TRUSTED_HOSTS.loadFromFile();
  }
  mbedtls_sha256_init(&shaContext_);
  if (store_) store_->begin();
  setState(State::STARTING);

  ble_ = std::make_unique<BleTransferRuntime>(*this);
  if (!ble_->begin()) {
    setError("Could not start BLE");
    return;
  }

  setState(State::ADVERTISING);
  publishStatus();
}

void BleTransferActivity::onExit() {
  Activity::onExit();
  resetTransfer(true);
  // The staged library listing, the uploaded progress batch and its result
  // document are scratch files for one session only.
  if (store_) store_->end();
  if (Storage.exists(CATALOG_PATH)) Storage.remove(CATALOG_PATH);
  if (Storage.exists(CATALOG_PART_PATH)) Storage.remove(CATALOG_PART_PATH);
  if (Storage.exists(LIBRARY_INDEX_PATH)) Storage.remove(LIBRARY_INDEX_PATH);
  if (Storage.exists(PROGRESS_BATCH_PATH)) Storage.remove(PROGRESS_BATCH_PATH);
  if (Storage.exists(PROGRESS_RESULT_PATH)) Storage.remove(PROGRESS_RESULT_PATH);
  if (ble_) {
    ble_->end();
    ble_.reset();
  }
  mbedtls_sha256_free(&shaContext_);
}

void BleTransferActivity::loop() {
  processBleEvents();

  // The pairing prompts belong to the session, not to either face of it, so they
  // are handled before the store gets a look at the frame.
  if (store_ && state_ != State::SAVE_HOST_PROMPT && state_ != State::FORGET_HOST_PROMPT) {
    if (pendingCommit_) {
      pendingCommit_ = false;
      processCommit();
      return;
    }
    store_->tick();
    if (store_->handleInput()) return;
    if (statusDirty_) publishStatus();
    return;
  }

  if (state_ == State::FIRMWARE_CONFIRM) {
    handleFirmwareConfirm();
    return;
  }
  if (state_ == State::SAVE_HOST_PROMPT) {
    handleSaveHostPrompt();
    return;
  }
  if (state_ == State::FORGET_HOST_PROMPT) {
    handleForgetHostPrompt();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Left) && BLE_TRUSTED_HOSTS.hasHosts() &&
      (state_ == State::ADVERTISING || state_ == State::CONNECTED)) {
    promptSelection_ = 0;
    setState(State::FORGET_HOST_PROMPT);
    return;
  }
  if (pendingCommit_) {
    pendingCommit_ = false;
    processCommit();
    return;
  }
  if (state_ == State::SENDING && downloadOpen_) {
    if (statusDirty_) publishStatus();
    pumpDownload();
    return;
  }
  if (statusDirty_) publishStatus();
}

void BleTransferActivity::enqueueBleEvent(BleEvent event) {
  if (!eventMutex_) return;
  const size_t eventBytes = event.value.size();
  xSemaphoreTake(eventMutex_, portMAX_DELAY);
  if (bleEventOverflow_ || bleEvents_.size() >= MAX_QUEUED_BLE_EVENTS ||
      queuedBleEventBytes_ + eventBytes > MAX_QUEUED_BLE_EVENT_BYTES) {
    bleEventOverflow_ = true;
    queuedBleEventBytes_ = 0;
    bleEvents_.clear();
  } else {
    queuedBleEventBytes_ += eventBytes;
    bleEvents_.push_back(std::move(event));
  }
  xSemaphoreGive(eventMutex_);
}

void BleTransferActivity::enqueueBleConnected() { enqueueBleEvent({BleEventType::CONNECTED, {}}); }

void BleTransferActivity::enqueueBleDisconnected() { enqueueBleEvent({BleEventType::DISCONNECTED, {}}); }

void BleTransferActivity::enqueueControlWrite(const std::string& value) {
  enqueueBleEvent({BleEventType::CONTROL, value});
}

void BleTransferActivity::enqueueDataWrite(const std::string& value) { enqueueBleEvent({BleEventType::DATA, value}); }

void BleTransferActivity::processBleEvents() {
  while (true) {
    BleEvent event;
    bool hasEvent = false;
    bool hasOverflow = false;
    if (eventMutex_) {
      xSemaphoreTake(eventMutex_, portMAX_DELAY);
      if (bleEventOverflow_) {
        bleEventOverflow_ = false;
        queuedBleEventBytes_ = 0;
        bleEvents_.clear();
        hasOverflow = true;
      }
      if (!bleEvents_.empty()) {
        event = std::move(bleEvents_.front());
        queuedBleEventBytes_ -= event.value.size();
        bleEvents_.pop_front();
        hasEvent = true;
      }
      xSemaphoreGive(eventMutex_);
    }
    if (hasOverflow) {
      resetTransfer(true);
      setError("BLE event queue overflow");
      return;
    }
    if (!hasEvent) return;

    switch (event.type) {
      case BleEventType::CONNECTED:
        onBleConnected();
        break;
      case BleEventType::DISCONNECTED:
        onBleDisconnected();
        break;
      case BleEventType::CONTROL:
        onControlWrite(event.value);
        break;
      case BleEventType::DATA:
        onDataWrite(event.value);
        break;
    }
  }
}

void BleTransferActivity::onBleConnected() {
  helloAccepted_ = false;
  trustedHelloAccepted_ = false;
  trustedHostName_.clear();
  setState(State::CONNECTED);
}

void BleTransferActivity::onBleDisconnected() {
  if (state_ == State::UPDATING || state_ == State::RESTARTING) return;

  // The Store is live or it is nothing: with the link gone there is no
  // catalogue to show, so it drops what it had rather than leaving a page on
  // screen that no longer describes anything reachable.
  if (store_) store_->onAppGone();

  if (transferOpen_ || downloadOpen_) {
    const bool keepPartialUpload = transferOpen_ && uploadResumable_;
    resetTransfer(!keepPartialUpload);
    if (keepPartialUpload) {
      helloAccepted_ = false;
      trustedHelloAccepted_ = false;
      trustedHostName_.clear();
      deviceNonce_ = makeNonceHex();
      setState(State::ADVERTISING);
      if (ble_) ble_->startAdvertising();
      return;
    }
    setError("client disconnected");
    return;
  }
  if (transferKind_ == TransferKind::FIRMWARE &&
      (state_ == State::FIRMWARE_CONFIRM ||
       (state_ == State::SAVE_HOST_PROMPT && pendingFinalState_ == State::FIRMWARE_CONFIRM))) {
    resetTransfer(true);
  }
  helloAccepted_ = false;
  trustedHelloAccepted_ = false;
  trustedHostName_.clear();
  deviceNonce_ = makeNonceHex();
  setState(State::ADVERTISING);
  if (ble_) ble_->startAdvertising();
}

void BleTransferActivity::onControlWrite(const std::string& value) {
  if (state_ == State::UPDATING || state_ == State::RESTARTING) return;

  JsonDocument doc;
  const DeserializationError parseError = deserializeJson(doc, value.data(), value.size());
  if (parseError) {
    setError("invalid control JSON");
    return;
  }

  const std::string op = doc["op"] | "";
  if (op == "hello") {
    const int version = doc["version"] | 0;
    const std::string code = doc["code"] | "";
    const std::string hostId = doc["host_id"] | "";
    const std::string response = toLowerAscii(doc["response"] | "");
    if (version != 1) {
      setError("unsupported protocol version");
      return;
    }

    if (!hostId.empty() && !response.empty()) {
      if (!isSafeHostId(hostId) || !isHexString(response, 64)) {
        setError("invalid trusted host auth");
        return;
      }
      const BleTrustedHost* host = BLE_TRUSTED_HOSTS.findHost(hostId);
      if (!host) {
        setError("unknown trusted host");
        return;
      }
      const std::string expected = hmacSha256Hex(host->secret, trustedHostMessage(deviceNonce_, hostId));
      if (expected.empty() || !constantTimeEquals(expected, response)) {
        setError("invalid trusted host auth");
        return;
      }
      helloAccepted_ = true;
      trustedHelloAccepted_ = true;
      trustedHostName_ = host->name.empty() ? hostId : host->name;
      deviceNonce_ = makeNonceHex();
      setState(State::CONNECTED);
      // The gate is the only thing the Store was waiting for: ask for page one.
      if (store_) store_->onAppReady();
      return;
    }

    if (code != sessionCode_) {
      setError("invalid session code");
      return;
    }
    if (!setPendingTrustedHost(doc["pair_host_id"] | "", doc["pair_host_name"] | "",
                               toLowerAscii(doc["pair_secret"] | ""))) {
      setError("invalid trusted host setup");
      return;
    }
    helloAccepted_ = true;
    trustedHelloAccepted_ = false;
    setState(State::CONNECTED);
    if (store_) store_->onAppReady();
    return;
  }

  if (!helloAccepted_) {
    setError("session code required");
    return;
  }

  if (op == "save_host") {
    setError("save_host requires completed upload");
    return;
  }

  if (op == "catalog_error") {
    // The app giving up early rather than letting the device sit out the whole
    // timeout: Calibre unreachable, the book withdrawn, a query that failed. It
    // must name the request it is failing, or it is ignored.
    if (!store_) {
      setError("store not open", false);
      return;
    }
    const uint32_t req = doc["req"] | 0u;
    std::string message = doc["error"] | "";
    if (message.size() > 96) message.resize(96);
    store_->onAppError(req, message);
    return;
  }

  if (op == "set_time") {
    // There is no NTP in a build without the network stack, so this is the only
    // way the device learns the *real* date: HalClock::begin() has already
    // started the RTC from the firmware's build epoch, which is a lower bound
    // that keeps saves stamped but drifts behind wall time until this arrives.
    // A client's time always wins -- it is the more accurate of the two.
    if (!halClock.isAvailable()) {
      setError("no clock on this device");
      return;
    }
    const int64_t epoch = doc["epoch"] | static_cast<int64_t>(0);
    if (epoch < static_cast<int64_t>(HalClock::MIN_VALID_EPOCH) ||
        epoch >= static_cast<int64_t>(HalClock::MAX_VALID_EPOCH)) {
      setError("invalid epoch");
      return;
    }
    if (!halClock.setEpoch(static_cast<uint32_t>(epoch))) {
      setError("could not set clock");
      return;
    }
    LOG_INF("BLE", "Clock set by client to %lu", static_cast<unsigned long>(epoch));
    // No state change: the acknowledgement is `device_time` in the status the
    // client is already subscribed to, which is also how it detects drift.
    statusDirty_ = true;
    requestUpdate();
    return;
  }

  if (op == "start_put") {
    resetTransfer(true);

    const std::string kind = doc["kind"] | "";
    // Present only on an answer to a `pending` request. Zero everywhere else,
    // which is exactly what an ordinary Bluetooth Transfer upload sends.
    const uint32_t responseReq = doc["req"] | 0u;
    fileName_ = doc["name"] | "";
    expectedSize_ = doc["size"] | 0;
    expectedSha256_ = toLowerAscii(doc["sha256"] | "");
    uploadResumable_ = doc["resume"] | false;
    uploadChunkSize_ = doc["chunk_size"] | 0;
    uploadAckBytes_ = doc["ack_bytes"] | BLE_PROGRESS_STATUS_INTERVAL_BYTES;
    transferKind_ = TransferKind::NONE;

    if (!isHexSha256(expectedSha256_)) {
      setError("invalid sha256");
      return;
    }
    if (uploadResumable_ && uploadChunkSize_ == 0) {
      setError("invalid resume chunk size");
      return;
    }
    if (uploadAckBytes_ < BLE_UPLOAD_ACK_BYTES_MIN || uploadAckBytes_ > BLE_UPLOAD_ACK_BYTES_MAX) {
      setError("invalid ack window");
      return;
    }

    if (kind == "book") {
      if (!isSafeBleBookName(fileName_)) {
        setError("unsafe book filename");
        return;
      }
      if (store_) {
        // In the Store, a book upload is only ever the answer to a
        // `catalog_fetch` the device published. It must name that request and
        // that exact file: the user asked for one book, and an unsolicited push
        // must not land on the card in its place.
        if (!store_->acceptsResponse(responseReq, BleStoreController::PendingOp::FETCH)) {
          setError("stale request", false);
          return;
        }
        if (storeExpectedBook_.empty() || fileName_ != storeExpectedBook_) {
          setError("unexpected book", false);
          return;
        }
      }
      if (expectedSize_ == 0 || expectedSize_ > MAX_BLE_BOOK_BYTES) {
        setError("invalid book size");
        return;
      }
      if (!Storage.exists(BOOKS_ROOT) && !Storage.mkdir(BOOKS_ROOT)) {
        setError("could not create books directory");
        return;
      }
      transferKind_ = TransferKind::BOOK;
      partPath_ = std::string(BOOKS_ROOT) + "/.ble-" + fileName_ + ".part";
      finalPath_ = std::string(BOOKS_ROOT) + "/" + fileName_;
      if (Storage.exists(finalPath_.c_str())) {
        setError("exists");
        return;
      }
    } else if (kind == "bmp") {
      if (!isSafeBleBmpName(fileName_)) {
        setError("unsafe bmp filename");
        return;
      }
      if (expectedSize_ == 0 || expectedSize_ > MAX_BLE_BMP_BYTES) {
        setError("invalid bmp size");
        return;
      }
      if (!Storage.exists(PICTURES_ROOT) && !Storage.mkdir(PICTURES_ROOT)) {
        setError("could not create pictures directory");
        return;
      }
      transferKind_ = TransferKind::BMP;
      partPath_ = std::string(PICTURES_ROOT) + "/.ble-" + fileName_ + ".part";
      finalPath_ = std::string(PICTURES_ROOT) + "/" + fileName_;
      if (Storage.exists(finalPath_.c_str())) {
        setError("exists");
        return;
      }
    } else if (kind == "progress") {
      // The batch has no user-facing name and never lands on the shelf: it is
      // staged at a fixed scratch path, parsed on commit, and deleted.
      if (expectedSize_ == 0 || expectedSize_ > MAX_BLE_PROGRESS_BYTES) {
        setError("invalid progress size");
        return;
      }
      if (!Storage.ensureDirectoryExists(CROSSPOINT_ROOT)) {
        setError("could not create data directory");
        return;
      }
      transferKind_ = TransferKind::PROGRESS;
      fileName_ = PROGRESS_BATCH_NAME;
      partPath_ = PROGRESS_BATCH_PART_PATH;
      finalPath_ = PROGRESS_BATCH_PATH;
    } else if (kind == "catalog_page" || kind == "catalog_detail") {
      // The answer to the question in the last `status` notification. It rides
      // the ordinary upload path -- framing, credit flow control, SHA-256,
      // commit -- and adds only the request id that ties it to the question.
      const bool detail = kind == "catalog_detail";
      if (!store_) {
        setError("store not open", false);
        return;
      }
      if (!store_->acceptsResponse(
              responseReq, detail ? BleStoreController::PendingOp::DETAIL : BleStoreController::PendingOp::PAGE)) {
        // A reply to a question the device has already given up on or moved past.
        // Refused without disturbing whatever is on screen now.
        setError("stale request", false);
        return;
      }
      if (expectedSize_ == 0 || expectedSize_ > BleCatalog::MAX_CONTAINER_BYTES) {
        setError("invalid catalog size");
        return;
      }
      if (!Storage.ensureDirectoryExists(STORE_ROOT)) {
        setError("could not create store directory");
        return;
      }
      transferKind_ = detail ? TransferKind::CATALOG_DETAIL : TransferKind::CATALOG_PAGE;
      fileName_ = CATALOG_NAME;
      partPath_ = CATALOG_PART_PATH;
      finalPath_ = CATALOG_PATH;
    } else if (kind == "firmware") {
      if (!isSafeBleFirmwareName(fileName_)) {
        setError("unsafe firmware filename");
        return;
      }
      const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
      if (!dest) {
        setError("no update partition");
        return;
      }
      if (expectedSize_ < MIN_BLE_FIRMWARE_BYTES || expectedSize_ > dest->size) {
        setError("invalid firmware size");
        return;
      }
      if (!Storage.ensureDirectoryExists(BLE_OTA_ROOT)) {
        setError("could not create ota directory");
        return;
      }
      transferKind_ = TransferKind::FIRMWARE;
      partPath_ = BLE_OTA_PART_PATH;
      finalPath_ = BLE_OTA_FINAL_PATH;
    } else {
      setError("unsupported transfer kind");
      return;
    }

    mbedtls_sha256_starts(&shaContext_, 0);
    shaActive_ = true;
    receivedBytes_ = 0;
    expectedSequence_ = 0;
    if (uploadResumable_ && Storage.exists(partPath_.c_str())) {
      HalFile partialFile;
      if (!Storage.openFileForRead("BLE", partPath_, partialFile)) {
        setError("could not inspect partial transfer");
        resetTransfer(true);
        return;
      }
      const size_t partialSize = partialFile.fileSize();
      partialFile.close();
      if (partialSize > expectedSize_ ||
          (partialSize > 0 && partialSize < expectedSize_ && (partialSize % uploadChunkSize_) != 0)) {
        setError("partial transfer mismatch");
        resetTransfer(true);
        return;
      }
      if (!hashExistingPrefix(partPath_, partialSize, shaContext_)) {
        setError("could not hash partial transfer");
        resetTransfer(true);
        return;
      }
      uploadFile_ = Storage.open(partPath_.c_str(), O_RDWR);
      if (!uploadFile_ || !uploadFile_.seek(partialSize)) {
        setError("could not resume transfer file");
        resetTransfer(true);
        return;
      }
      receivedBytes_ = partialSize;
      expectedSequence_ = static_cast<uint32_t>(partialSize / uploadChunkSize_);
    } else {
      if (Storage.exists(partPath_.c_str())) Storage.remove(partPath_.c_str());
      if (!Storage.openFileForWrite("BLE", partPath_, uploadFile_)) {
        setError("could not open transfer file");
        return;
      }
    }
    transferOpen_ = true;
    removePartOnExit_ = true;
    lastProgressStatusBytes_ = receivedBytes_;
    lastDisplayProgressBytes_ = receivedBytes_;
    setState(State::RECEIVING);
    // The store's own deadline ends here: from now on the transfer path reports
    // progress and owns the failure, so the screen shows bytes rather than a
    // countdown.
    if (store_ && transferKind_ == TransferKind::BOOK) store_->onFetchStarted();
    return;
  }

  if (op == "start_get") {
    resetTransfer(true);
    const int64_t offsetValue = doc["offset"] | 0;
    const int64_t chunkSizeValue = doc["chunk_size"] | static_cast<int64_t>(BLE_DOWNLOAD_CHUNK_BYTES);
    if (offsetValue < 0 ||
        static_cast<uint64_t>(offsetValue) > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      setError("invalid download offset");
      return;
    }
    if (chunkSizeValue < static_cast<int64_t>(BLE_DOWNLOAD_CHUNK_BYTES_MIN) ||
        chunkSizeValue > static_cast<int64_t>(BLE_DOWNLOAD_CHUNK_BYTES_MAX)) {
      setError("invalid download chunk size");
      return;
    }
    const std::string kind = doc["kind"] | "";
    if (kind == "crash_report") {
      startCrashReportDownload(static_cast<size_t>(offsetValue), static_cast<size_t>(chunkSizeValue));
      return;
    }
    if (kind == "library") {
      startLibraryDownload(static_cast<size_t>(offsetValue), static_cast<size_t>(chunkSizeValue));
      return;
    }
    if (kind == "progress_result") {
      startProgressResultDownload(static_cast<size_t>(offsetValue), static_cast<size_t>(chunkSizeValue));
      return;
    }
    setError("unsupported transfer kind");
    return;
  }

  if (op == "get_ack") {
    if (!downloadOpen_ || !downloadAwaitingAck_) {
      setError("no download pending");
      return;
    }
    const uint32_t sequence = doc["sequence"] | UINT32_MAX;
    if (sequence != pendingDownloadAck_) {
      setError("unexpected download ack");
      return;
    }
    downloadAwaitingAck_ = false;
    statusDirty_ = true;
    requestUpdate();
    return;
  }

  if (op == "commit") {
    if (!transferOpen_) {
      setError("no transfer open");
      return;
    }
    pendingCommit_ = true;
    setState(State::VERIFYING);
    return;
  }

  if (op == "cancel") {
    resetTransfer(true);
    setState(State::CONNECTED);
    return;
  }

  setError("unknown control op");
}

void BleTransferActivity::onDataWrite(const std::string& value) {
  if (!transferOpen_ || state_ != State::RECEIVING) return;
  if (value.size() <= sizeof(uint32_t)) {
    setError("invalid data frame");
    resetTransfer(true);
    return;
  }

  const uint32_t sequence = readLe32(value);
  if (sequence != expectedSequence_) {
    setError("unexpected data sequence");
    resetTransfer(true);
    return;
  }

  const uint8_t* payload = reinterpret_cast<const uint8_t*>(value.data() + sizeof(uint32_t));
  const size_t payloadSize = value.size() - sizeof(uint32_t);
  if (receivedBytes_ + payloadSize > expectedSize_) {
    setError("transfer too large");
    resetTransfer(true);
    return;
  }
  if (uploadFile_.write(payload, payloadSize) != payloadSize) {
    setError("transfer write failed");
    resetTransfer(true);
    return;
  }

  mbedtls_sha256_update(&shaContext_, payload, payloadSize);
  receivedBytes_ += payloadSize;
  expectedSequence_++;
  if (receivedBytes_ == expectedSize_ || receivedBytes_ - lastProgressStatusBytes_ >= uploadAckBytes_) {
    lastProgressStatusBytes_ = receivedBytes_;
    statusDirty_ = true;
  }
  const size_t displayInterval = transferKind_ == TransferKind::FIRMWARE ? BLE_FIRMWARE_PROGRESS_DISPLAY_INTERVAL_BYTES
                                                                         : BLE_PROGRESS_DISPLAY_INTERVAL_BYTES;
  if (receivedBytes_ == expectedSize_ || receivedBytes_ - lastDisplayProgressBytes_ >= displayInterval) {
    lastDisplayProgressBytes_ = receivedBytes_;
    // The store paints its own progress bar, on the same cadence the transfer
    // screen repaints on.
    if (store_ && transferKind_ == TransferKind::BOOK) store_->onFetchProgress(receivedBytes_, expectedSize_);
    requestUpdate();
  }
}

void BleTransferActivity::processCommit() {
  if (!transferOpen_) return;
  setState(State::VERIFYING);

  uploadFile_.flush();
  uploadFile_.close();
  transferOpen_ = false;

  if (receivedBytes_ != expectedSize_) {
    setError("size mismatch");
    resetTransfer(true);
    return;
  }

  uint8_t digest[32];
  mbedtls_sha256_finish(&shaContext_, digest);
  shaActive_ = false;
  if (sha256ToHex(digest) != expectedSha256_) {
    setError("sha256 mismatch");
    resetTransfer(true);
    return;
  }

  if ((transferKind_ == TransferKind::BOOK || transferKind_ == TransferKind::BMP) &&
      Storage.exists(finalPath_.c_str())) {
    setError("exists");
    resetTransfer(true);
    return;
  }
  if ((transferKind_ == TransferKind::FIRMWARE || transferKind_ == TransferKind::PROGRESS ||
       transferKind_ == TransferKind::CATALOG_PAGE || transferKind_ == TransferKind::CATALOG_DETAIL) &&
      Storage.exists(finalPath_.c_str()) && !Storage.remove(finalPath_.c_str())) {
    setError("could not replace staged upload");
    resetTransfer(true);
    return;
  }
  if (!Storage.rename(partPath_.c_str(), finalPath_.c_str())) {
    setError("could not finalize transfer");
    resetTransfer(true);
    return;
  }
  removePartOnExit_ = false;

  if (transferKind_ == TransferKind::CATALOG_PAGE || transferKind_ == TransferKind::CATALOG_DETAIL) {
    // Only now that the whole container is on the card and its SHA-256 checks
    // out is it unpacked: the header is read (kilobytes), each cover is copied
    // out to its own small file, and the staged blob is deleted. Nothing larger
    // than one 512-byte buffer is ever resident.
    const bool detail = transferKind_ == TransferKind::CATALOG_DETAIL;
    removePartOnExit_ = false;
    if (store_) store_->onCatalogCommitted(finalPath_.c_str(), detail);
    resetTransfer(false);
    return;
  }

  if (transferKind_ == TransferKind::BOOK || transferKind_ == TransferKind::BMP) {
    savedPath_ = finalPath_;
    if (transferKind_ == TransferKind::BOOK) clearBookCache(savedPath_);
    // Told before completeFinalState(): that may stop on the save-host prompt,
    // and the book is on the card either way.
    if (store_ && transferKind_ == TransferKind::BOOK) {
      storeExpectedBook_.clear();
      store_->onFetchSaved(savedPath_);
    }
    completeFinalState(State::SAVED);
    return;
  }

  if (transferKind_ == TransferKind::PROGRESS) {
    // Only now that the whole batch is on disk and its SHA-256 checks out does
    // anything get written underneath a book.
    processProgressBatch();
    return;
  }

  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    setError("no update partition");
    return;
  }
  LOG_INF("BLE", "validating staged firmware: %s (%u bytes)", finalPath_.c_str(), static_cast<unsigned>(expectedSize_));
  const firmware_flash::Result validateRes = firmware_flash::validateImageFile(finalPath_.c_str(), dest->size);
  if (validateRes != firmware_flash::Result::OK) {
    Storage.remove(finalPath_.c_str());
    setError(std::string("invalid firmware: ") + firmware_flash::resultName(validateRes));
    return;
  }
  flashWrittenBytes_ = 0;
  promptSelection_ = 0;
  completeFinalState(State::FIRMWARE_CONFIRM);
  requestUpdateAndWait();
}

void BleTransferActivity::startFileDownload(const char* path, const char* name, const TransferKind kind,
                                            const size_t offset, const size_t chunkSize) {
  if (!Storage.exists(path)) {
    setError("not_found");
    return;
  }
  if (!Storage.openFileForRead("BLE", path, downloadFile_)) {
    setError("could not open download");
    return;
  }

  fileName_ = name;
  transferKind_ = kind;
  expectedSize_ = downloadFile_.fileSize();
  if (offset > expectedSize_) {
    downloadFile_.close();
    setError("invalid download offset");
    return;
  }
  if (offset < expectedSize_ && offset % chunkSize != 0) {
    downloadFile_.close();
    setError("unaligned download offset");
    return;
  }
  if (!downloadFile_.seek(offset)) {
    downloadFile_.close();
    setError("could not seek download");
    return;
  }
  sentBytes_ = offset;
  downloadSequence_ = static_cast<uint32_t>(offset / chunkSize);
  pendingDownloadAck_ = 0;
  downloadAwaitingAck_ = false;
  downloadChunkSize_ = chunkSize;
  lastProgressStatusBytes_ = sentBytes_;
  downloadOpen_ = true;
  setState(State::SENDING);
}

void BleTransferActivity::startCrashReportDownload(const size_t offset, const size_t chunkSize) {
  startFileDownload(CRASH_REPORT_PATH, CRASH_REPORT_NAME, TransferKind::CRASH_REPORT, offset, chunkSize);
}

void BleTransferActivity::startLibraryDownload(const size_t offset, const size_t chunkSize) {
  // Offset 0 means a fresh listing, so rebuild it: a client must never resume
  // onto a document that changed underneath it. A non-zero offset can only refer
  // to the file staged by that same start_get.
  if (offset == 0) {
    // Walking the shelf is seconds of SD work -- one metadata cache open per
    // book -- so say so on screen and over BLE before blocking on it.
    setState(State::PREPARING);
    publishStatus();
    requestUpdateAndWait();

    BookLibraryIndex::Stats stats;
    if (!BookLibraryIndex::build(BOOKS_ROOT, LIBRARY_INDEX_PATH, &stats)) {
      setError("could not build library index");
      return;
    }
    LOG_DBG("BLE", "Library index staged: %u books", static_cast<unsigned>(stats.books));
  } else if (!Storage.exists(LIBRARY_INDEX_PATH)) {
    setError("not_found");
    return;
  }
  startFileDownload(LIBRARY_INDEX_PATH, LIBRARY_INDEX_NAME, TransferKind::LIBRARY, offset, chunkSize);
}

void BleTransferActivity::startProgressResultDownload(const size_t offset, const size_t chunkSize) {
  startFileDownload(PROGRESS_RESULT_PATH, PROGRESS_RESULT_NAME, TransferKind::PROGRESS_RESULT, offset, chunkSize);
}

void BleTransferActivity::processProgressBatch() {
  progressEntries_ = 0;
  progressApplied_ = 0;

  // No open-book hazard to guard against here. Reaching this screen goes through
  // ActivityManager::goToBluetoothTransfer(), which calls replaceActivity()
  // (ActivityManager.cpp) -- that drops the current activity AND the whole
  // stack, so a reader has already run onExit(), written its final progress.bin
  // and been destroyed before BLE advertising ever starts. There is no
  // in-memory reader position for these writes to fight, and when the user next
  // opens the book the reader loads the position from disk, which is what was
  // just written. Nothing is deferred and nothing is rejected on this account.
  HalFile in;
  if (!Storage.openFileForRead("BLE", PROGRESS_BATCH_PATH, in)) {
    setError("could not read progress batch");
    resetTransfer(true);
    return;
  }
  if (Storage.exists(PROGRESS_RESULT_PATH)) Storage.remove(PROGRESS_RESULT_PATH);
  HalFile out;
  if (!Storage.openFileForWrite("BLE", PROGRESS_RESULT_PATH, out)) {
    in.close();
    setError("could not stage progress results");
    resetTransfer(true);
    return;
  }

  ProgressBatchReader reader(in);
  std::string objectText;
  bool ok = out.print("[") == 1;
  bool first = true;
  bool malformed = false;

  while (ok) {
    const auto next = reader.next(objectText);
    if (next == ProgressBatchReader::Next::PARSE_ERROR) {
      malformed = true;
      break;
    }
    if (next == ProgressBatchReader::Next::END) break;
    if (progressEntries_ >= MAX_PROGRESS_ENTRIES) {
      malformed = true;
      break;
    }
    progressEntries_++;
    // Each entry is several SD operations (existence check, sidecar read,
    // atomic write); a large batch would otherwise outlast the watchdog window.
    resetTaskWatchdogIfSubscribed();

    std::string filename;
    auto result = BookProgressSync::ApplyResult::INVALID;
    JsonDocument entry;
    if (deserializeJson(entry, objectText) == DeserializationError::Ok) {
      filename = entry["filename"] | "";
      const std::string location = toLowerAscii(entry["location"] | "");
      const int64_t timestamp = entry["timestamp"] | static_cast<int64_t>(0);
      // One bad entry costs that entry only. A book the phone knows about but
      // this card does not is the ordinary case, not a failed batch.
      if (isSafeBleBookRelativePath(filename) && timestamp > 0 &&
          timestamp <= static_cast<int64_t>(UINT32_MAX)) {
        result = BookProgressSync::applyProgress(BOOKS_ROOT, filename, location,
                                                 static_cast<uint32_t>(timestamp));
      }
    }
    if (result == BookProgressSync::ApplyResult::APPLIED) progressApplied_++;

    JsonDocument resultDoc;
    // Echoed back so the client can match outcomes to entries without relying on
    // array position; empty when the entry was too malformed to name a book.
    resultDoc["filename"] = filename;
    resultDoc["result"] = BookProgressSync::applyResultName(result);
    String json;
    serializeJson(resultDoc, json);
    if (!first && out.print(",") != 1) {
      ok = false;
      break;
    }
    first = false;
    if (out.print(json) != json.length()) ok = false;
  }

  if (ok) ok = out.print("]") == 1;
  out.flush();
  out.close();
  in.close();
  // The uploaded batch is scratch; the result document stays until the transfer
  // screen closes so the client can fetch it.
  Storage.remove(PROGRESS_BATCH_PATH);
  removePartOnExit_ = false;

  if (!ok || malformed) {
    Storage.remove(PROGRESS_RESULT_PATH);
    progressEntries_ = 0;
    progressApplied_ = 0;
    setError(malformed ? "malformed progress batch" : "could not write progress results");
    resetTransfer(true);
    return;
  }

  LOG_INF("BLE", "Progress batch: %u entries, %u applied", static_cast<unsigned>(progressEntries_),
          static_cast<unsigned>(progressApplied_));
  completeFinalState(State::SAVED);
}

void BleTransferActivity::pumpDownload() {
  if (!downloadOpen_ || downloadAwaitingAck_) return;

  std::array<uint8_t, sizeof(uint32_t) + BLE_DOWNLOAD_CHUNK_BYTES> frame = {};
  frame[0] = static_cast<uint8_t>(downloadSequence_ & 0xFF);
  frame[1] = static_cast<uint8_t>((downloadSequence_ >> 8) & 0xFF);
  frame[2] = static_cast<uint8_t>((downloadSequence_ >> 16) & 0xFF);
  frame[3] = static_cast<uint8_t>((downloadSequence_ >> 24) & 0xFF);

  const int read = downloadFile_.read(frame.data() + sizeof(uint32_t), downloadChunkSize_);
  if (read < 0) {
    downloadFile_.close();
    downloadOpen_ = false;
    setError("download read failed");
    return;
  }
  if (read == 0) {
    downloadFile_.close();
    downloadOpen_ = false;
    setState(State::SENT);
    return;
  }

  ble_->notifyData(frame.data(), sizeof(uint32_t) + static_cast<size_t>(read));
  sentBytes_ += static_cast<size_t>(read);
  pendingDownloadAck_ = downloadSequence_;
  downloadAwaitingAck_ = true;
  downloadSequence_++;
  if (sentBytes_ == expectedSize_ || sentBytes_ - lastProgressStatusBytes_ >= BLE_PROGRESS_STATUS_INTERVAL_BYTES) {
    lastProgressStatusBytes_ = sentBytes_;
    statusDirty_ = true;
    requestUpdate();
  }
}

bool BleTransferActivity::setPendingTrustedHost(const std::string& hostId, const std::string& hostName,
                                                const std::string& secret) {
  candidateHostId_.clear();
  candidateHostName_.clear();
  candidateHostSecret_.clear();
  if (hostId.empty() && secret.empty()) return true;
  if (!isSafeHostId(hostId) || !isHexString(secret, BLE_SHARED_SECRET_HEX_BYTES)) return false;
  candidateHostId_ = hostId;
  candidateHostName_ = sanitizeHostName(hostName);
  candidateHostSecret_ = secret;
  return true;
}

void BleTransferActivity::completeFinalState(const State finalState) {
  hostPaired_ = false;
  hostPairSkipped_ = false;
  if (!trustedHelloAccepted_ && !candidateHostId_.empty() && !candidateHostSecret_.empty()) {
    pendingFinalState_ = finalState;
    promptSelection_ = 0;
    setState(State::SAVE_HOST_PROMPT);
    return;
  }
  setState(finalState);
}

void BleTransferActivity::handleFirmwareConfirm() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    if (promptSelection_ > 0) {
      promptSelection_--;
      requestUpdate();
    }
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (promptSelection_ < 1) {
      promptSelection_++;
      requestUpdate();
    }
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (promptSelection_ != 0) {
      Storage.remove(finalPath_.c_str());
      setError("firmware update rejected");
      return;
    }

    flashWrittenBytes_ = 0;
    lastFirmwareFlashRenderedPercent_ = 101;
    setState(State::UPDATING);
    publishStatus();
    requestUpdateAndWait();
    const auto progressCb = +[](const size_t written, const size_t total, void* ctx) {
      auto* self = static_cast<BleTransferActivity*>(ctx);
      self->flashWrittenBytes_ = written;
      self->expectedSize_ = total;
      self->statusDirty_ = true;
      self->publishStatus();
      const unsigned int pct = total > 0 ? static_cast<unsigned int>((written * 100) / total) : 0;
      if (pct != self->lastFirmwareFlashRenderedPercent_) {
        self->lastFirmwareFlashRenderedPercent_ = pct;
        self->renderFirmwareUpdating();
      }
    };
    firmware_flash::Result flashRes = firmware_flash::Result::READ_FAIL;
    {
      RenderLock lock(*this);
      flashRes = firmware_flash::flashFromSdPath(finalPath_.c_str(), progressCb, this, true);
    }
    if (flashRes != firmware_flash::Result::OK) {
      Storage.remove(finalPath_.c_str());
      setError(std::string("firmware update failed: ") + firmware_flash::resultName(flashRes));
      return;
    }
    flashWrittenBytes_ = expectedSize_;
    setState(State::RESTARTING);
    publishStatus();
    delay(750);
    ESP.restart();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    Storage.remove(finalPath_.c_str());
    setError("firmware update rejected");
  }
}

void BleTransferActivity::handleSaveHostPrompt() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    if (promptSelection_ > 0) {
      promptSelection_--;
      requestUpdate();
    }
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (promptSelection_ < 1) {
      promptSelection_++;
      requestUpdate();
    }
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (promptSelection_ == 0) {
      RenderLock lock(*this);
      hostPaired_ = BLE_TRUSTED_HOSTS.addOrReplaceHost(
          BleTrustedHost{candidateHostId_, candidateHostName_, candidateHostSecret_});
      hostPairSkipped_ = !hostPaired_;
    } else {
      hostPairSkipped_ = true;
    }
    candidateHostId_.clear();
    candidateHostName_.clear();
    candidateHostSecret_.clear();
    setState(pendingFinalState_);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    hostPairSkipped_ = true;
    candidateHostId_.clear();
    candidateHostName_.clear();
    candidateHostSecret_.clear();
    setState(pendingFinalState_);
  }
}

void BleTransferActivity::handleForgetHostPrompt() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    if (promptSelection_ > 0) {
      promptSelection_--;
      requestUpdate();
    }
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (promptSelection_ < 1) {
      promptSelection_++;
      requestUpdate();
    }
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (promptSelection_ == 1) {
      RenderLock lock(*this);
      if (!BLE_TRUSTED_HOSTS.clearAll()) {
        setError("could not forget trusted host");
        return;
      }
    }
    setState(State::ADVERTISING);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    setState(State::ADVERTISING);
  }
}

void BleTransferActivity::resetTransfer(const bool removePart) {
  if (shaActive_) {
    mbedtls_sha256_free(&shaContext_);
    mbedtls_sha256_init(&shaContext_);
    shaActive_ = false;
  }
  if (uploadFile_) uploadFile_.close();
  if (downloadFile_) downloadFile_.close();
  if (removePart && transferKind_ == TransferKind::FIRMWARE && state_ != State::UPDATING &&
      state_ != State::RESTARTING && !finalPath_.empty() && Storage.exists(finalPath_.c_str())) {
    Storage.remove(finalPath_.c_str());
  }
  if (removePart && removePartOnExit_ && !partPath_.empty() && Storage.exists(partPath_.c_str())) {
    Storage.remove(partPath_.c_str());
  }

  fileName_.clear();
  partPath_.clear();
  finalPath_.clear();
  expectedSha256_.clear();
  savedPath_.clear();
  transferKind_ = TransferKind::NONE;
  pendingFinalState_ = State::CONNECTED;
  expectedSize_ = 0;
  receivedBytes_ = 0;
  sentBytes_ = 0;
  flashWrittenBytes_ = 0;
  lastProgressStatusBytes_ = 0;
  lastDisplayProgressBytes_ = 0;
  uploadChunkSize_ = 0;
  uploadAckBytes_ = BLE_PROGRESS_STATUS_INTERVAL_BYTES;
  downloadChunkSize_ = BLE_DOWNLOAD_CHUNK_BYTES;
  expectedSequence_ = 0;
  downloadSequence_ = 0;
  pendingDownloadAck_ = 0;
  transferOpen_ = false;
  downloadOpen_ = false;
  downloadAwaitingAck_ = false;
  pendingCommit_ = false;
  removePartOnExit_ = false;
  uploadResumable_ = false;
}

void BleTransferActivity::setState(const State state) {
  state_ = state;
  statusDirty_ = true;
  requestUpdate();
}

void BleTransferActivity::setError(const std::string& error) { setError(error, true); }

void BleTransferActivity::setError(const std::string& error, const bool notifyStore) {
  errorMessage_ = error;
  state_ = State::ERROR;
  statusDirty_ = true;
  requestUpdate();
  // A refused answer to a question the device is no longer asking is a normal
  // race, reported to the app in `status` and nowhere else. Everything else the
  // Store user needs to see.
  if (notifyStore && store_) store_->onTransferError(error);
}

void BleTransferActivity::storePublishStatus() {
  statusDirty_ = true;
  publishStatus();
  requestUpdate();
}

void BleTransferActivity::storeRepaint() { requestUpdate(); }

void BleTransferActivity::storeArmBookFetch(const std::string& filename) { storeExpectedBook_ = filename; }

void BleTransferActivity::storeFinish() { finish(); }

void BleTransferActivity::storeOpenBook(const std::string& path) { activityManager.goToReader(path); }

void BleTransferActivity::noteBleMtu(const uint16_t mtu) { negotiatedMtu_.store(mtu, std::memory_order_relaxed); }

size_t BleTransferActivity::notifyCapBytes() const {
  uint16_t mtu = negotiatedMtu_.load(std::memory_order_relaxed);
  // 0 means no exchange has happened (or the peer has gone). Assume the floor
  // rather than the 517 this server asked for: an optimistic guess here is
  // exactly how a document ends up truncated on the wire.
  if (mtu < BLE_ATT_MTU_MINIMUM) mtu = BLE_ATT_MTU_MINIMUM;
  const size_t cap = static_cast<size_t>(mtu) - BLE_ATT_NOTIFY_OVERHEAD;
  return cap < BLE_STATUS_NOTIFY_MAX_BYTES ? cap : BLE_STATUS_NOTIFY_MAX_BYTES;
}

std::string BleTransferActivity::buildNotifyJson(const size_t capBytes) const {
  for (unsigned detail = STATUS_DETAIL_MAX;; --detail) {
    std::string json = buildStatusJson(StatusScope::NOTIFY, detail);
    if (json.size() <= capBytes) return json;
    if (detail == 0) break;
  }
  // Not even `{"state":"..."}` fits -- a 23-byte MTU with a long state name.
  // An empty object is still a valid document and still rings the doorbell;
  // truncating one would hand the client a parse error instead.
  return "{}";
}

void BleTransferActivity::publishStatus() {
  statusDirty_ = false;
  if (!ble_) return;
  ble_->publish(buildStatusJson(StatusScope::READ, STATUS_DETAIL_MAX), buildNotifyJson(notifyCapBytes()));
}

std::string BleTransferActivity::buildStatusJson(const StatusScope scope, const unsigned detail) const {
  JsonDocument doc;
  const std::string state = stateName(state_);
  // The notification is a doorbell, the read is authoritative. A READ carries
  // the whole session; a NOTIFY carries what the client cannot cheaply re-derive
  // and drops the rest until it fits the ATT payload. Everything dropped here is
  // still one GATT read away, and the client already has to read to get the
  // capability lists it saw at connect time.
  const bool full = (scope == StatusScope::READ);
  const bool wantSession = full || detail >= 5;   // session-constant capability facts
  const bool wantIdentity = full || detail >= 4;  // who this device is, and to whom
  const bool wantProgress = full || detail >= 2;  // byte counters and the error text
  const bool wantPending = full || detail >= 1;   // the store's request channel
  // Only below level 3 does the request shed the geometry and the deadline the
  // app builds its answer from; a GATT read still has them.
  const bool pendingTerse = !full && detail < 3;

  doc["state"] = state.c_str();
  if (wantSession) doc["protocol_version"] = 1;
  if (full) {
    doc["firmware_name"] = "CrossPoint Reader";
    // Read-only by design. A Web Bluetooth companion URL is a session constant
    // that no notification has any business carrying.
    doc["browser_companion_url"] = BLE_TRANSFER_WEB_URL;
    doc["firmware_ota_supported"] = true;
    doc["resume_supported"] = true;
    JsonArray uploadKinds = doc["upload_kinds"].to<JsonArray>();
    uploadKinds.add("book");
    uploadKinds.add("bmp");
    uploadKinds.add("firmware");
    uploadKinds.add("progress");
    uploadKinds.add("catalog_page");
    uploadKinds.add("catalog_detail");
    JsonArray downloadKinds = doc["download_kinds"].to<JsonArray>();
    downloadKinds.add("crash_report");
    downloadKinds.add("library");
    downloadKinds.add("progress_result");
  }
  // The store is a capability of this firmware, not of this screen: an app can
  // see it is supported while the user is still on the transfer screen.
  if (wantSession) doc["store_supported"] = true;
  if (mode_ == Mode::STORE && wantIdentity) doc["mode"] = "store";
  if (wantSession) {
    doc["clock_supported"] = halClock.isAvailable();
    // Omitted, never zeroed, when the device does not know the time: the client
    // uses its absence to decide it must send `set_time`, and its value to notice
    // drift. A `0` here would read as a real 1970 instant.
    uint32_t deviceEpoch = 0;
    if (halClock.getEpoch(deviceEpoch)) doc["device_time"] = deviceEpoch;
  }
  if (full) {
    doc["device_id"] = deviceId_.c_str();
    doc["device_nonce"] = deviceNonce_.c_str();
    doc["has_trusted_host"] = BLE_TRUSTED_HOSTS.hasHosts();
  }
  if (wantIdentity) {
    if (!trustedHostName_.empty()) doc["trusted_host"] = trustedHostName_.c_str();
    if (hostPaired_) doc["paired"] = true;
    if (hostPairSkipped_) doc["pairing"] = "skipped";
  }
  if (wantProgress && (expectedSize_ > 0 || state_ == State::SENDING || state_ == State::SENT)) {
    const std::string kind = transferKindName(transferKind_);
    if (!kind.empty()) doc["kind"] = kind.c_str();
    if (state_ == State::SAVED && transferKind_ == TransferKind::PROGRESS) {
      // A finished batch reports its outcome, not its byte count. Kept this
      // short deliberately: the per-entry outcomes are a `progress_result`
      // download precisely because a shelf-sized array fits in no notification.
      doc["entries"] = progressEntries_;
      doc["applied"] = progressApplied_;
    } else if (state_ == State::SENDING || state_ == State::SENT) {
      doc["sent"] = sentBytes_;
      doc["size"] = expectedSize_;
    } else if (state_ == State::UPDATING || state_ == State::RESTARTING) {
      doc["written"] = flashWrittenBytes_;
      doc["size"] = expectedSize_;
    } else {
      doc["received"] = receivedBytes_;
      if (uploadResumable_) doc["resumable"] = true;
      doc["ack_bytes"] = uploadAckBytes_;
      doc["size"] = expectedSize_;
    }
  }
  if (wantIdentity && state_ == State::SAVED && !savedPath_.empty()) {
    doc["name"] = fileName_.c_str();
    doc["path"] = savedPath_.c_str();
  }
  if (wantIdentity && state_ == State::SENT) doc["name"] = fileName_.c_str();
  // `state` already says ERROR; the message is the part that can be any length,
  // so it is the part that goes when the payload is tight.
  if (wantProgress && state_ == State::ERROR && !errorMessage_.empty()) doc["error"] = errorMessage_.c_str();
  // The request channel. When the device wants something from the app it says so
  // here, and the app answers with an upload naming the same `req`. Absent
  // whenever nothing is outstanding.
  if (wantPending && store_) store_->describePending(doc, pendingTerse);

  String output;
  serializeJson(doc, output);
  return output.c_str();
}

void BleTransferActivity::render(RenderLock&&) {
  if (state_ == State::FIRMWARE_CONFIRM) {
    renderFirmwareConfirm();
    return;
  }
  if (state_ == State::UPDATING) {
    renderFirmwareUpdating();
    return;
  }
  if (state_ == State::SAVE_HOST_PROMPT) {
    renderSaveHostPrompt();
    return;
  }
  if (state_ == State::FORGET_HOST_PROMPT) {
    renderForgetHostPrompt();
    return;
  }
  if (store_) {
    // The Store owns the whole screen in its mode; the session's own states
    // (receiving, error) are reported through it, not around it.
    store_->render(sessionCode_);
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLUETOOTH_TRANSFER));

  const int centerY = pageHeight / 2 - 30;
  std::string primary;
  std::string secondary;

  switch (state_) {
    case State::STARTING:
      primary = tr(STR_LOADING_POPUP);
      break;
    case State::ADVERTISING:
      primary = tr(STR_BLE_TRANSFER_READY);
      secondary = std::string(tr(STR_BLE_TRANSFER_CODE)) + sessionCode_;
      break;
    case State::CONNECTED:
      primary = tr(STR_CONNECTED);
      secondary = std::string(tr(STR_BLE_TRANSFER_CODE)) + sessionCode_;
      break;
    case State::RECEIVING: {
      primary = tr(STR_BLE_TRANSFER_RECEIVING);
      char buffer[48];
      snprintf(buffer, sizeof(buffer), "%u / %u bytes", static_cast<unsigned>(receivedBytes_),
               static_cast<unsigned>(expectedSize_));
      secondary = buffer;
      break;
    }
    case State::VERIFYING:
      primary = tr(STR_BLE_TRANSFER_VERIFYING);
      secondary = fileName_;
      break;
    case State::SAVED:
      if (transferKind_ == TransferKind::PROGRESS) {
        primary = tr(STR_BLE_TRANSFER_PROGRESS_SAVED);
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%u / %u", static_cast<unsigned>(progressApplied_),
                 static_cast<unsigned>(progressEntries_));
        secondary = buffer;
      } else {
        primary = tr(STR_BLE_TRANSFER_SAVED);
        secondary = savedPath_.empty() ? fileName_ : savedPath_;
      }
      break;
    case State::RESTARTING:
      primary = tr(STR_BLE_TRANSFER_RESTARTING);
      secondary = tr(STR_BLE_TRANSFER_FIRMWARE_UPDATED);
      break;
    case State::PREPARING:
      primary = tr(STR_BLE_TRANSFER_PREPARING_LIBRARY);
      break;
    case State::SENDING: {
      if (transferKind_ == TransferKind::LIBRARY) {
        primary = tr(STR_BLE_TRANSFER_SENDING_LIBRARY);
      } else if (transferKind_ == TransferKind::PROGRESS_RESULT) {
        primary = tr(STR_BLE_TRANSFER_SENDING_RESULTS);
      } else {
        primary = tr(STR_BLE_TRANSFER_SENDING);
      }
      char buffer[48];
      snprintf(buffer, sizeof(buffer), "%u / %u bytes", static_cast<unsigned>(sentBytes_),
               static_cast<unsigned>(expectedSize_));
      secondary = buffer;
      break;
    }
    case State::SENT:
      if (transferKind_ == TransferKind::LIBRARY) {
        primary = tr(STR_BLE_TRANSFER_LIBRARY_SENT);
      } else if (transferKind_ == TransferKind::PROGRESS_RESULT) {
        primary = tr(STR_BLE_TRANSFER_RESULTS_SENT);
      } else {
        primary = tr(STR_BLE_TRANSFER_SENT);
      }
      secondary = fileName_;
      break;
    case State::ERROR:
      primary = tr(STR_ERROR_MSG);
      secondary = errorMessage_;
      break;
    case State::FIRMWARE_CONFIRM:
    case State::UPDATING:
    case State::SAVE_HOST_PROMPT:
    case State::FORGET_HOST_PROMPT:
      return;
  }

  if (state_ == State::ADVERTISING || state_ == State::CONNECTED) {
    renderCompanionReady(primary, secondary);
    const char* forgetLabel = BLE_TRUSTED_HOSTS.hasHosts() ? tr(STR_FORGET_BUTTON) : "";
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", forgetLabel, "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  renderer.drawCenteredText(UI_10_FONT_ID, centerY, primary.c_str(), true, EpdFontFamily::BOLD);
  if (!secondary.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + renderer.getLineHeight(UI_10_FONT_ID) + 8, secondary.c_str());
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void BleTransferActivity::renderCompanionReady(const std::string& primary, const std::string& secondary) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int y = contentTop;

  renderer.drawCenteredText(UI_10_FONT_ID, y, primary.c_str(), true, EpdFontFamily::BOLD);
  y += lineHeight + metrics.verticalSpacing;
  renderer.drawCenteredText(SMALL_FONT_ID, y, BLE_TRANSFER_WEB_URL, true);
  y += renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;

  const int availableHeight = renderer.getScreenHeight() - y - metrics.verticalSpacing - lineHeight;
  const int qrSize = std::min({BLE_TRANSFER_QR_SIZE, pageWidth - metrics.contentSidePadding * 2, availableHeight});
  if (qrSize > 0) {
    const Rect qrBounds((pageWidth - qrSize) / 2, y, qrSize, qrSize);
    QrUtils::drawQrCode(renderer, qrBounds, BLE_TRANSFER_WEB_URL);
    y += qrSize + metrics.verticalSpacing;
  }
  if (!secondary.empty()) renderer.drawCenteredText(UI_10_FONT_ID, y, secondary.c_str(), true);
}

void BleTransferActivity::renderFirmwareConfirm() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height * 3) / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLUETOOTH_TRANSFER));
  renderer.drawCenteredText(UI_12_FONT_ID, top - 40, tr(STR_FIRMWARE_UPDATE_PROMPT), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, top, fileName_.empty() ? "firmware.bin" : fileName_.c_str());
  renderer.drawCenteredText(UI_10_FONT_ID, top + 40, tr(STR_BLE_TRANSFER_RESTART_AFTER_FLASH));

  const int buttonY = top + 80;
  constexpr int buttonWidth = 80;
  constexpr int buttonSpacing = 30;
  constexpr int totalWidth = buttonWidth * 2 + buttonSpacing;
  const int startX = (pageWidth - totalWidth) / 2;
  const std::string yesLabel = tr(STR_YES);
  const std::string noLabel = tr(STR_NO);
  const std::string yesText = promptSelection_ == 0 ? "[" + yesLabel + "]" : yesLabel;
  const std::string noText = promptSelection_ == 1 ? "[" + noLabel + "]" : noLabel;

  renderer.drawText(UI_10_FONT_ID, startX + (promptSelection_ == 0 ? 0 : 4), buttonY, yesText.c_str());
  renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing + (promptSelection_ == 1 ? 0 : 4), buttonY,
                    noText.c_str());

  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void BleTransferActivity::renderFirmwareUpdating() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLUETOOTH_TRANSFER));
  const int centerY = pageHeight / 2 - 54;
  renderer.drawCenteredText(UI_12_FONT_ID, centerY, tr(STR_BLE_TRANSFER_WRITING_FIRMWARE), true, EpdFontFamily::BOLD);

  const int percent = expectedSize_ > 0 ? static_cast<int>((flashWrittenBytes_ * 100) / expectedSize_) : 0;
  GUI.drawProgressBar(renderer,
                      Rect{metrics.contentSidePadding, centerY + 36, pageWidth - metrics.contentSidePadding * 2,
                           metrics.progressBarHeight},
                      percent, 100);

  char progress[32];
  const auto writtenTenthsMb = static_cast<unsigned>((static_cast<uint64_t>(flashWrittenBytes_) * 10) / (1024 * 1024));
  const auto totalTenthsMb = static_cast<unsigned>((static_cast<uint64_t>(expectedSize_) * 10) / (1024 * 1024));
  snprintf(progress, sizeof(progress), "%u.%u / %u.%u MB", writtenTenthsMb / 10, writtenTenthsMb % 10,
           totalTenthsMb / 10, totalTenthsMb % 10);
  renderer.drawCenteredText(UI_10_FONT_ID, centerY + 92, progress);
  renderer.drawCenteredText(UI_10_FONT_ID, centerY + 122, tr(STR_BLE_TRANSFER_FLASH_TIME_HINT));
  renderer.drawCenteredText(UI_10_FONT_ID, centerY + 150, tr(STR_FIRMWARE_UPDATE_DO_NOT_POWER_OFF));
  renderer.displayBuffer();
}

void BleTransferActivity::renderSaveHostPrompt() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height * 3) / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLUETOOTH_TRANSFER));
  renderer.drawCenteredText(UI_12_FONT_ID, top - 40, tr(STR_BLE_SAVE_HOST), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, top,
                            candidateHostName_.empty() ? "Trusted host" : candidateHostName_.c_str());
  renderer.drawCenteredText(UI_10_FONT_ID, top + 40, tr(STR_BLE_SAVE_HOST_PROMPT));

  const int buttonY = top + 80;
  constexpr int buttonWidth = 60;
  constexpr int buttonSpacing = 30;
  constexpr int totalWidth = buttonWidth * 2 + buttonSpacing;
  const int startX = (pageWidth - totalWidth) / 2;
  const std::string yesLabel = tr(STR_YES);
  const std::string noLabel = tr(STR_NO);
  const std::string yesText = promptSelection_ == 0 ? "[" + yesLabel + "]" : yesLabel;
  const std::string noText = promptSelection_ == 1 ? "[" + noLabel + "]" : noLabel;

  renderer.drawText(UI_10_FONT_ID, startX + (promptSelection_ == 0 ? 0 : 4), buttonY, yesText.c_str());
  renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing + (promptSelection_ == 1 ? 0 : 4), buttonY,
                    noText.c_str());

  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void BleTransferActivity::renderForgetHostPrompt() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height * 3) / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLUETOOTH_TRANSFER));
  renderer.drawCenteredText(UI_12_FONT_ID, top - 40, tr(STR_BLE_FORGET_HOST), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_BLE_FORGET_HOST_PROMPT));

  const int buttonY = top + 80;
  constexpr int buttonWidth = 120;
  constexpr int buttonSpacing = 30;
  constexpr int totalWidth = buttonWidth * 2 + buttonSpacing;
  const int startX = (pageWidth - totalWidth) / 2;
  const std::string cancelLabel = tr(STR_CANCEL);
  const std::string forgetLabel = tr(STR_FORGET_BUTTON);
  const std::string cancelText = promptSelection_ == 0 ? "[" + cancelLabel + "]" : cancelLabel;
  const std::string forgetText = promptSelection_ == 1 ? "[" + forgetLabel + "]" : forgetLabel;

  renderer.drawText(UI_10_FONT_ID, startX + (promptSelection_ == 0 ? 0 : 4), buttonY, cancelText.c_str());
  renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing + (promptSelection_ == 1 ? 0 : 4), buttonY,
                    forgetText.c_str());

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
