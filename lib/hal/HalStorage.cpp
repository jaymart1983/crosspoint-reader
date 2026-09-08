#include "HalStorage.h"

#include <FS.h>  // need to be included before SdFat.h for compatibility with FS.h's File class
#include <HalClock.h>
#include <Logging.h>
#include <SDCardManager.h>
#if FREEINK_CAP_USB_MSC
#include <UsbMassStorage.h>
#endif

#include <cassert>

#define SDCard SDCardManager::getInstance()

namespace {
#if FREEINK_CAP_USB_MSC
freeink::UsbMassStorage usbMassStorage;
#endif

// FAT epoch: 1980-01-01T00:00:00. FAT stores no timezone, so the fields written
// here and the fields read back in modifiedEpoch() are the same clock's local
// reading -- the only thing anything compares is two of them against each other.
constexpr uint32_t FAT_EPOCH_UTC = 315532800UL;  // 1980-01-01T00:00:00Z
constexpr uint32_t SECONDS_PER_DAY = 86400UL;

bool isLeapYear(const uint32_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

uint32_t daysInMonth(const uint32_t year, const uint32_t month) {
  static constexpr uint8_t DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && isLeapYear(year)) return 29;
  return DAYS[month - 1];
}

// Without this, every file the device writes carries SdFat's fixed default date
// and "newest added" is unanswerable for anything that did not arrive over USB
// from a host PC -- which is every book the Store pulls down. The clock is
// seeded from the firmware's build epoch at boot (see HalClock::begin), so this
// has a usable date from the first write on a brand-new device.
void sdDateTimeCallback(uint16_t* date, uint16_t* time, uint8_t* ms10) {
  if (ms10) *ms10 = 0;
  uint32_t epoch = 0;
  if (!halClock.getEpoch(epoch) || epoch < FAT_EPOCH_UTC) {
    // No usable clock: leave FAT's own zero rather than invent a date. SdFat
    // reads a zero date as "not set", which modifiedEpoch() reports as unknown.
    if (date) *date = 0;
    if (time) *time = 0;
    return;
  }

  uint32_t days = (epoch - FAT_EPOCH_UTC) / SECONDS_PER_DAY;
  uint32_t remainder = (epoch - FAT_EPOCH_UTC) % SECONDS_PER_DAY;
  uint32_t year = 1980;
  while (true) {
    const uint32_t yearDays = isLeapYear(year) ? 366 : 365;
    if (days < yearDays) break;
    days -= yearDays;
    year++;
  }
  uint32_t month = 1;
  while (days >= daysInMonth(year, month)) {
    days -= daysInMonth(year, month);
    month++;
  }
  // FAT packs the year as an offset from 1980 in 7 bits, so it runs out in 2108.
  if (year > 2107) year = 2107;
  if (date) *date = FS_DATE(static_cast<uint16_t>(year), static_cast<uint8_t>(month), static_cast<uint8_t>(days + 1));
  if (time) {
    *time = FS_TIME(static_cast<uint8_t>(remainder / 3600), static_cast<uint8_t>((remainder % 3600) / 60),
                    static_cast<uint8_t>(remainder % 60));
  }
}
}  // namespace

HalStorage HalStorage::instance;

HalStorage::HalStorage() {
  // Recursive so the same task can re-enter StorageLock without self-deadlock.
  // openFileForRead/Write take the lock and then assign to a HalFile&
  // out-param; if that out-param already held an Impl, its destructor takes
  // the lock again to close the prior FsFile under serialization (see
  // HalFile::Impl::~Impl below). Priority inheritance still applies to
  // recursive mutexes.
  storageMutex = xSemaphoreCreateRecursiveMutex();
  assert(storageMutex != nullptr);
}

// begin() and ready() are only called from setup, no need to acquire mutex for them

bool HalStorage::begin() {
  // Registered before the mount so the very first file written after boot is
  // already stamped. Global to SdFat, hence set once here rather than per open.
  FsDateTime::setCallback(sdDateTimeCallback);
  return SDCard.begin();
}

bool HalStorage::ready() const { return SDCard.ready(); }

uint64_t HalStorage::totalBytes() const { return SDCard.sdTotalBytes(); }

uint64_t HalStorage::usedBytes() { return SDCard.sdUsedBytes(); }

uint64_t HalStorage::freeBytes() {
  const uint64_t total = SDCard.sdTotalBytes();
  const uint64_t used = SDCard.sdUsedBytes();
  return used >= total ? 0 : total - used;
}

// For the rest of the methods, we acquire the mutex to ensure thread safety

class HalStorage::StorageLock {
 public:
  StorageLock() { xSemaphoreTakeRecursive(HalStorage::getInstance().storageMutex, portMAX_DELAY); }
  ~StorageLock() { xSemaphoreGiveRecursive(HalStorage::getInstance().storageMutex); }
};

void HalStorage::prepareForDeepSleep() {
  StorageLock lock;
  SDCard.shutdown();
}

#if FREEINK_CAP_USB_MSC && !FREEINK_SD_SDMMC
#error "USB Drive requires an SDMMC-backed storage profile"
#endif

bool HalStorage::beginUsbDrive() {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  auto* const blockDevice = SDCard.detachFilesystemForRawAccess();
  if (!blockDevice) {
    LOG_ERR("USB", "USB Drive requires a mounted SDMMC filesystem");
    return false;
  }

  if (!usbMassStorage.begin(blockDevice)) {
    LOG_ERR("USB", "USB Drive MSC initialization failed");
    if (!SDCard.begin()) {
      LOG_ERR("USB", "Unable to remount SD card after USB Drive startup failure");
    }
    return false;
  }
  return true;
#else
  return false;
#endif
}

bool HalStorage::disconnectUsbDriveHost() {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  return usbMassStorage.disconnectHost();
#else
  return false;
#endif
}

void HalStorage::endUsbDrive() {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  usbMassStorage.end();
#endif
}

UsbDriveState HalStorage::usbDriveState() const {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  switch (usbMassStorage.state()) {
    case freeink::UsbMassStorageState::WaitingForHost:
      return UsbDriveState::WaitingForHost;
    case freeink::UsbMassStorageState::Connected:
    case freeink::UsbMassStorageState::Accessed:
      return UsbDriveState::Connected;
    case freeink::UsbMassStorageState::Ejected:
      return UsbDriveState::Ejected;
    case freeink::UsbMassStorageState::Disconnected:
      return UsbDriveState::Disconnected;
    case freeink::UsbMassStorageState::IoError:
      return UsbDriveState::IoError;
    case freeink::UsbMassStorageState::Idle:
      break;
  }
#endif
  return UsbDriveState::Unsupported;
}

#define HAL_STORAGE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;               \
  return SDCard.method(__VA_ARGS__);

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  HAL_STORAGE_WRAPPED_CALL(listFiles, path, maxFiles);
}

String HalStorage::readFile(const char* path) { HAL_STORAGE_WRAPPED_CALL(readFile, path); }

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  HAL_STORAGE_WRAPPED_CALL(readFileToStream, path, out, chunkSize);
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  HAL_STORAGE_WRAPPED_CALL(readFileToBuffer, path, buffer, bufferSize, maxBytes);
}

bool HalStorage::writeFile(const char* path, const String& content) {
  HAL_STORAGE_WRAPPED_CALL(writeFile, path, content);
}

bool HalStorage::ensureDirectoryExists(const char* path) { HAL_STORAGE_WRAPPED_CALL(ensureDirectoryExists, path); }

class HalFile::Impl {
 public:
  Impl(FsFile&& fsFile) : file(std::move(fsFile)) {}
  // SdFat is not thread-safe; FsFile::close() touches SD/SPI and must run
  // under StorageLock or it races SdSpiCard::m_spiActive across tasks and
  // trips FreeRTOS's xTaskPriorityDisinherit assert. The FsFile member
  // destructor (DESTRUCTOR_CLOSES_FILE=1) will close() again after the lock
  // releases, but close() on an already-closed FsFile is a no-op. See SdFat
  // issue #518 and the HAL note in CLAUDE.md.
  ~Impl() {
    HalStorage::StorageLock lock;
    file.close();
  }
  FsFile file;
};

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) = default;
HalFile& HalFile::operator=(HalFile&&) = default;

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  return HalFile(std::make_unique<HalFile::Impl>(SDCard.open(path, oflag)));
}

bool HalStorage::mkdir(const char* path, const bool pFlag) { HAL_STORAGE_WRAPPED_CALL(mkdir, path, pFlag); }

bool HalStorage::exists(const char* path) { HAL_STORAGE_WRAPPED_CALL(exists, path); }

bool HalStorage::remove(const char* path) { HAL_STORAGE_WRAPPED_CALL(remove, path); }
bool HalStorage::rename(const char* oldPath, const char* newPath) {
  HAL_STORAGE_WRAPPED_CALL(rename, oldPath, newPath);
}

bool HalStorage::rmdir(const char* path) { HAL_STORAGE_WRAPPED_CALL(rmdir, path); }

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForRead(moduleName, path, fsFile);
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  return ok;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForWrite(moduleName, path, fsFile);
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  return ok;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) { HAL_STORAGE_WRAPPED_CALL(removeDir, path); }

// HalFile implementation
// Allow doing file operations while ensuring thread safety via HalStorage's mutex.
// Please keep the list below in sync with the HalFile.h header

#define HAL_FILE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;            \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

#define HAL_FILE_FORWARD_CALL(method, ...) \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

void HalFile::flush() { HAL_FILE_WRAPPED_CALL(flush, ); }
size_t HalFile::getName(char* name, size_t len) { HAL_FILE_WRAPPED_CALL(getName, name, len); }
size_t HalFile::size() { HAL_FILE_FORWARD_CALL(size, ); }              // already thread-safe, no need to wrap
size_t HalFile::fileSize() { HAL_FILE_FORWARD_CALL(fileSize, ); }      // already thread-safe, no need to wrap
uint64_t HalFile::fileSize64() { HAL_FILE_FORWARD_CALL(fileSize, ); }  // already thread-safe, no need to wrap
bool HalFile::seek(size_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seek64(uint64_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seekCur(int64_t offset) { HAL_FILE_WRAPPED_CALL(seekCur, offset); }
bool HalFile::seekSet(size_t offset) { HAL_FILE_WRAPPED_CALL(seekSet, offset); }
int HalFile::available() const { HAL_FILE_WRAPPED_CALL(available, ); }
size_t HalFile::position() const { HAL_FILE_WRAPPED_CALL(position, ); }
int HalFile::read(void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(read, buf, count); }
int HalFile::read() { HAL_FILE_WRAPPED_CALL(read, ); }
size_t HalFile::write(const uint8_t* buf, size_t count) { HAL_FILE_WRAPPED_CALL(write, buf, count); }
size_t HalFile::write(const void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(write, buf, count); }
size_t HalFile::write(uint8_t b) { HAL_FILE_WRAPPED_CALL(write, b); }
bool HalFile::rename(const char* newPath) { HAL_FILE_WRAPPED_CALL(rename, newPath); }
bool HalFile::isDirectory() const { HAL_FILE_FORWARD_CALL(isDirectory, ); }  // already thread-safe, no need to wrap

bool HalFile::modifiedEpoch(uint32_t& epochUtc) {
  uint16_t date = 0;
  uint16_t time = 0;
  {
    HalStorage::StorageLock lock;
    assert(impl != nullptr);
    if (!impl->file.getModifyDateTime(&date, &time)) return false;
  }
  // FAT's "never set" is a zero date, which FS_YEAR would read as 1980-00-00.
  if (date == 0) return false;
  const uint32_t year = FS_YEAR(date);
  const uint32_t month = FS_MONTH(date);
  const uint32_t day = FS_DAY(date);
  if (year < 1980 || month < 1 || month > 12 || day < 1 || day > 31) return false;

  uint32_t days = 0;
  for (uint32_t y = 1980; y < year; y++) days += isLeapYear(y) ? 366 : 365;
  for (uint32_t m = 1; m < month; m++) days += daysInMonth(year, m);
  days += day - 1;
  epochUtc = FAT_EPOCH_UTC + days * SECONDS_PER_DAY + FS_HOUR(time) * 3600UL + FS_MINUTE(time) * 60UL + FS_SECOND(time);
  return true;
}
void HalFile::rewindDirectory() { HAL_FILE_WRAPPED_CALL(rewindDirectory, ); }
bool HalFile::close() { HAL_FILE_WRAPPED_CALL(close, ); }
HalFile HalFile::openNextFile() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return HalFile(std::make_unique<Impl>(impl->file.openNextFile()));
}
bool HalFile::isOpen() const { return impl != nullptr && impl->file.isOpen(); }  // already thread-safe, no need to wrap
HalFile::operator bool() const { return isOpen(); }
