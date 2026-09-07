#pragma once

#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace ProgressFile {

// The saved position itself lives in `<cachePath>/progress.bin`, whose layout is
// private to each reader activity (EPUB writes 4/6/10 bytes, XTC and TXT write
// 4). "When was that position saved" lives in a sidecar next to it rather than
// inside it, for two reasons:
//
//  1. All three readers dispatch on the exact byte length of progress.bin --
//     EpubReaderActivity accepts 4, 6 or 10 and reads the 10-byte form as
//     "position + visible-text offset". Appending four timestamp bytes would
//     turn a 6-byte EPUB position into the 10-byte form and resume the book at a
//     text offset that is really a clock reading. Length is load-bearing; it
//     cannot carry a trailer.
//  2. Firmware that predates this file must keep opening books written by
//     firmware that has it. An ignored extra file is compatible in both
//     directions; a longer progress.bin is not.
//
// Backwards compatibility, therefore: every progress.bin written before this
// existed has no sidecar, and readSavedTime() reports that as "unknown". Unknown
// is NOT epoch 0 -- callers must treat it as older than any real timestamp
// rather than as a 1970 save that everything beats by definition.
constexpr const char* SIDECAR_NAME = "/progress.time";

// magic "CPPT" (CrossPoint Progress Time) + version, so a truncated or
// scribbled-on sidecar reads as unknown instead of as a wrong instant.
constexpr uint8_t SIDECAR_MAGIC[4] = {'C', 'P', 'P', 'T'};
constexpr uint8_t SIDECAR_VERSION = 1;
constexpr size_t SIDECAR_BYTES = 9;  // magic(4) + version(1) + epoch LE(4)

// Reads the UTC epoch the position in `<cachePath>/progress.bin` was saved at.
//
// Returns false for "unknown": no sidecar (a pre-existing book, or one saved
// while the device had no clock), a short read, a bad magic/version, or an
// implausible epoch. Never yields 0 as a timestamp.
inline bool readSavedTime(const std::string& cachePath, uint32_t& epochUtc) {
  HalFile f;
  if (!Storage.openFileForRead("PRG", cachePath + SIDECAR_NAME, f)) return false;
  uint8_t buf[SIDECAR_BYTES] = {};
  const int read = f.read(buf, sizeof(buf));
  f.close();
  if (read != static_cast<int>(SIDECAR_BYTES)) return false;
  for (size_t i = 0; i < sizeof(SIDECAR_MAGIC); i++) {
    if (buf[i] != SIDECAR_MAGIC[i]) return false;
  }
  if (buf[4] != SIDECAR_VERSION) return false;
  const uint32_t epoch = static_cast<uint32_t>(buf[5]) | (static_cast<uint32_t>(buf[6]) << 8) |
                         (static_cast<uint32_t>(buf[7]) << 16) | (static_cast<uint32_t>(buf[8]) << 24);
  if (!HalClock::isPlausibleEpoch(epoch)) return false;
  epochUtc = epoch;
  return true;
}

// Drops the sidecar, so the saved position reads as "saved at an unknown time".
//
// This is what a save made while the device has no working clock does. Leaving a
// stale sidecar behind would be actively wrong: it would claim a position the
// user reached just now was reached at whatever the last known time was, and an
// incoming sync newer than that would silently overwrite genuinely fresher
// reading.
inline void clearSavedTime(const std::string& cachePath) {
  const std::string path = cachePath + SIDECAR_NAME;
  if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
}

// Stamps `<cachePath>/progress.bin` as having been saved at `epochUtc`.
//
// Written in place rather than through a temp-and-rename: the record is nine
// bytes inside a single sector, and -- unlike progress.bin, whose corruption
// stranded books (issue #2275) -- a torn write here fails the magic/length check
// and degrades to "unknown", which every caller already handles as the oldest
// possible timestamp. The failure mode is a skipped sync, not a lost book.
inline bool writeSavedTime(const std::string& cachePath, const uint32_t epochUtc) {
  if (!HalClock::isPlausibleEpoch(epochUtc)) return false;
  const uint8_t record[SIDECAR_BYTES] = {SIDECAR_MAGIC[0],
                                         SIDECAR_MAGIC[1],
                                         SIDECAR_MAGIC[2],
                                         SIDECAR_MAGIC[3],
                                         SIDECAR_VERSION,
                                         static_cast<uint8_t>(epochUtc & 0xFF),
                                         static_cast<uint8_t>((epochUtc >> 8) & 0xFF),
                                         static_cast<uint8_t>((epochUtc >> 16) & 0xFF),
                                         static_cast<uint8_t>((epochUtc >> 24) & 0xFF)};
  const std::string path = cachePath + SIDECAR_NAME;
  HalFile f;
  if (!Storage.openFileForWrite("PRG", path, f)) {
    LOG_ERR("PRG", "Could not open progress timestamp for write: %s", path.c_str());
    return false;
  }
  const size_t written = f.write(record, sizeof(record));
  f.flush();
  f.close();
  if (written != sizeof(record)) {
    LOG_ERR("PRG", "Short write saving progress timestamp to %s", path.c_str());
    Storage.remove(path.c_str());
    return false;
  }
  return true;
}

// Writes `len` bytes of reader progress to `<cachePath>/progress.bin` without
// ever leaving the canonical file half-written.
//
// The bytes go to a temporary `progress.bin.tmp` first; only once that is fully
// written and closed is it renamed over progress.bin. An interrupted write
// (power loss or a crash mid-SPI) therefore damages only the throwaway temp file.
// Previously a truncate-in-place write that was cut short left progress.bin with
// a broken FAT cluster chain that the firmware could neither rewrite nor clear,
// stranding the book on an old page (issue #2275).
//
// This is crash-safe, not metadata-atomic: on FAT the replace is remove + rename,
// two separate directory operations, so a crash between them can leave neither
// file -- which simply reads as "no saved progress" on next launch, never a
// corrupt or unclearable file. The point is that progress.bin is never torn.
//
// Note: this prevents corruption on a healthy card going forward. It cannot
// repair an already-corrupted progress.bin -- removing the stale file may itself
// fail at the FAT level, in which case recovery still requires fsck on a host.
//
// Returns true only if the new progress.bin is fully in place.
inline bool writeBytesAtomic(const std::string& cachePath, const uint8_t* data, size_t len) {
  const std::string finalPath = cachePath + "/progress.bin";
  const std::string tmpPath = cachePath + "/progress.bin.tmp";

  {
    HalFile f;
    if (!Storage.openFileForWrite("PRG", tmpPath, f)) {
      LOG_ERR("PRG", "Could not open temp progress file for write: %s", tmpPath.c_str());
      return false;
    }
    const size_t written = f.write(data, len);
    if (written != len) {
      LOG_ERR("PRG", "Short write saving progress to %s: %u/%u bytes", tmpPath.c_str(), (unsigned)written,
              (unsigned)len);
      return false;
    }
    f.flush();
    // f (the temp file) is closed at scope exit (DESTRUCTOR_CLOSES_FILE=1) before
    // the rename below -- SdFat must not rename a path that still has an open FsFile.
  }

  // SdFat's rename does not overwrite an existing destination, so drop the old
  // canonical file first. The brief window where neither file exists reads as
  // "no saved progress" on next launch -- never a corrupt, unclearable file.
  Storage.remove(finalPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    LOG_ERR("PRG", "Failed to rename temp progress into place: %s", finalPath.c_str());
    return false;
  }
  return true;
}

// Saves progress and stamps it with `savedAtEpoch`.
//
// The stamp is written after progress.bin is in place, so a crash in between
// leaves a real position with an unknown time -- which reads as "oldest", the
// conservative direction: a sync will decline to overwrite it only when it is
// itself older, and the user loses a comparison, never a page.
inline bool writeAtomicAt(const std::string& cachePath, const uint8_t* data, const size_t len,
                          const uint32_t savedAtEpoch) {
  if (!writeBytesAtomic(cachePath, data, len)) return false;
  if (!writeSavedTime(cachePath, savedAtEpoch)) clearSavedTime(cachePath);
  return true;
}

// Saves progress, stamped with the device's own clock.
//
// This is the reader's entry point. When the device does not know what time it
// is (no RTC, or an RTC that was never set -- there is no NTP in a build without
// the network stack, so the phone has to set it over BLE), any existing stamp is
// dropped rather than kept: the position is new, the time is not known, and
// saying "unknown" is the only honest answer.
inline bool writeAtomic(const std::string& cachePath, const uint8_t* data, const size_t len) {
  if (!writeBytesAtomic(cachePath, data, len)) return false;
  uint32_t epoch = 0;
  if (halClock.getEpoch(epoch)) {
    writeSavedTime(cachePath, epoch);
  } else {
    clearSavedTime(cachePath);
  }
  return true;
}

}  // namespace ProgressFile
