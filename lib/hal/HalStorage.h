#pragma once

#include <Print.h>
#include <common/FsApiConstants.h>  // for oflag_t
#include <freertos/semphr.h>

#include <memory>
#include <string>
#include <vector>

class HalFile;

enum class UsbDriveState : uint8_t {
  Unsupported,
  WaitingForHost,
  Connected,
  Ejected,
  Disconnected,
  IoError,
};

class HalStorage {
 public:
  HalStorage();
  bool begin();
  bool ready() const;
  // Stop the SD card for deep sleep: unmount, stop the SDMMC host, and release
  // the bus pads (no-op on SPI boards). Call only after all file users have
  // stopped; open HalFiles become invalid. A deep-sleep wake resets the MCU and
  // mounts storage again through begin().
  void prepareForDeepSleep();
  // USB Drive exclusively owns the SD card while active. Callers must stop
  // all filesystem work before beginUsbDrive(), then reboot after endUsbDrive().
  // True from the moment the device commits to handing the card over until it
  // has it back. Distinct from HalGPIO::isUsbConnected(), which is a VBUS pin --
  // that says a cable carries power, not that the host has the card, and cannot
  // tell a wall charger from a data cable.
  //
  // It is set BEFORE beginUsbDrive() rather than derived from usbDriveState()
  // because nothing may be drawn once the filesystem is detached (fonts live on
  // it), so the one repaint that shows the indicator has to happen while the
  // handoff is still only an intention.
  bool usbDriveHandoffPending() const { return usbDriveHandoffPending_; }
  void setUsbDriveHandoffPending(bool pending) { usbDriveHandoffPending_ = pending; }
  bool beginUsbDrive();
  bool disconnectUsbDriveHost();
  void endUsbDrive();
  UsbDriveState usbDriveState() const;
  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200);
  // Read the entire file at `path` into a String. Returns empty string on failure.
  String readFile(const char* path);
  // Low-memory helpers:
  // Stream the file contents to a `Print` (e.g. `Serial`, or any `Print`-derived object).
  // Returns true on success, false on failure.
  bool readFileToStream(const char* path, Print& out, size_t chunkSize = 256);
  // Read up to `bufferSize-1` bytes into `buffer`, null-terminating it. Returns bytes read.
  size_t readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes = 0);
  // Write a string to `path` on the SD card. Overwrites existing file.
  // Returns true on success.
  bool writeFile(const char* path, const String& content);
  // Ensure a directory exists, creating it if necessary. Returns true on success.
  bool ensureDirectoryExists(const char* path);

  HalFile open(const char* path, const oflag_t oflag = O_RDONLY);
  bool mkdir(const char* path, const bool pFlag = true);
  bool exists(const char* path);
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  bool rmdir(const char* path);

  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForRead(const char* moduleName, const String& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const String& path, HalFile& file);
  bool removeDir(const char* path);

  static HalStorage& getInstance() { return instance; }

  class StorageLock;  // private class, used internally

 private:
  bool usbDriveHandoffPending_ = false;
  static HalStorage instance;

  bool initialized = false;
  SemaphoreHandle_t storageMutex = nullptr;
};

#define Storage HalStorage::getInstance()

class HalFile : public Print {
  friend class HalStorage;
  class Impl;
  std::unique_ptr<Impl> impl;
  explicit HalFile(std::unique_ptr<Impl> impl);

 public:
  HalFile();
  ~HalFile();
  HalFile(HalFile&&);
  HalFile& operator=(HalFile&&);
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  void flush();
  size_t getName(char* name, size_t len);
  size_t size();
  size_t fileSize();
  uint64_t fileSize64();
  bool seek(size_t pos);
  bool seek64(uint64_t pos);
  bool seekCur(int64_t offset);
  bool seekSet(size_t offset);
  int available() const;
  size_t position() const;
  int read(void* buf, size_t count);
  int read();  // read a single byte
  size_t write(const uint8_t* buf, size_t count) override;
  size_t write(const void* buf, size_t count);
  size_t write(uint8_t b) override;
  bool rename(const char* newPath);
  bool isDirectory() const;
  // FAT modification date/time as UTC epoch seconds, for "which book arrived
  // most recently". False when the entry carries no plausible date -- a card
  // written by firmware older than the SD date callback (see HalStorage::begin)
  // stamps every file with SdFat's fixed default, and a caller must treat that
  // as "unknown", never as a real instant. FAT has no timezone, so the stored
  // value is read back as the local time it was written in; the reader only
  // ever compares two of them against each other.
  bool modifiedEpoch(uint32_t& epochUtc);
  void rewindDirectory();
  bool close();
  HalFile openNextFile();
  bool isOpen() const;
  operator bool() const;
};

// Downstream code must use Storage instead of SdMan
#ifdef SdMan
#undef SdMan
#endif
