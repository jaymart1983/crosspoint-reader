#include "BookProgressSync.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include <cctype>

#include "activities/reader/ProgressFile.h"

namespace {

constexpr const char* TAG = "BSYN";
constexpr const char* CACHE_ROOT = "/.crosspoint";

int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

namespace BookProgressSync {

const char* applyResultName(const ApplyResult result) {
  switch (result) {
    case ApplyResult::APPLIED:
      return "applied";
    case ApplyResult::SKIPPED_OLDER:
      return "skipped_older";
    case ApplyResult::NOT_FOUND:
      return "not_found";
    case ApplyResult::UNSUPPORTED:
      return "unsupported";
    case ApplyResult::INVALID:
      return "invalid";
    case ApplyResult::WRITE_FAILED:
      return "write_failed";
  }
  return "invalid";
}

bool isSupportedBookName(const std::string& fileName) {
  return FsHelpers::hasEpubExtension(fileName) || FsHelpers::hasXtcExtension(fileName) ||
         FsHelpers::hasTxtExtension(fileName) || FsHelpers::hasMarkdownExtension(fileName);
}

std::string cachePathForBook(const std::string& fullPath) {
  // Each constructor only hashes the path -- no file is opened -- so this is
  // cheap enough to run per book while walking a whole shelf.
  if (FsHelpers::hasEpubExtension(fullPath)) return Epub(fullPath, CACHE_ROOT).getCachePath();
  if (FsHelpers::hasXtcExtension(fullPath)) return Xtc(fullPath, CACHE_ROOT).getCachePath();
  if (FsHelpers::hasTxtExtension(fullPath) || FsHelpers::hasMarkdownExtension(fullPath)) {
    return Txt(fullPath, CACHE_ROOT).getCachePath();
  }
  return {};
}

bool readProgressBlob(const std::string& cachePath, uint8_t* out, size_t& len) {
  len = 0;
  if (cachePath.empty()) return false;
  HalFile f;
  if (!Storage.openFileForRead(TAG, cachePath + "/progress.bin", f)) return false;
  // One byte past the largest known format, so a longer file is detected rather
  // than silently truncated into a valid-looking position.
  uint8_t buffer[MAX_PROGRESS_BYTES + 1] = {};
  const int read = f.read(buffer, sizeof(buffer));
  f.close();
  if (read <= 0 || read > static_cast<int>(MAX_PROGRESS_BYTES)) return false;
  len = static_cast<size_t>(read);
  for (size_t i = 0; i < len; i++) out[i] = buffer[i];
  return true;
}

std::string encodeLocation(const uint8_t* data, const size_t len) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.resize(len * 2);
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = hex[data[i] >> 4];
    out[i * 2 + 1] = hex[data[i] & 0x0F];
  }
  return out;
}

bool decodeLocation(const std::string& hex, uint8_t* out, size_t& len) {
  len = 0;
  if (hex.empty() || (hex.size() % 2) != 0 || hex.size() > MAX_PROGRESS_BYTES * 2) return false;
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int hi = hexValue(hex[i]);
    const int lo = hexValue(hex[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i / 2] = static_cast<uint8_t>((hi << 4) | lo);
  }
  len = hex.size() / 2;
  return true;
}

bool isValidLocationLength(const std::string& fileName, const size_t len) {
  if (FsHelpers::hasEpubExtension(fileName)) {
    // EpubReaderActivity::loadProgress dispatches on exactly these three: 4 is
    // the original spine+page form, 6 adds the chapter page count, 10 adds the
    // visible-text offset.
    return len == 4 || len == 6 || len == 10;
  }
  if (FsHelpers::hasXtcExtension(fileName)) return len == 4;
  if (FsHelpers::hasTxtExtension(fileName) || FsHelpers::hasMarkdownExtension(fileName)) return len == 4;
  return false;
}

ApplyResult applyProgress(const char* booksRoot, const std::string& relativePath, const std::string& locationHex,
                          const uint32_t timestamp) {
  if (!booksRoot || relativePath.empty()) return ApplyResult::INVALID;
  // Checked before anything else so a file the reader cannot open is reported as
  // exactly that, rather than as a malformed position.
  if (!isSupportedBookName(relativePath)) return ApplyResult::UNSUPPORTED;
  // A timestamp the device would not accept from its own clock is not a basis
  // for overwriting a position; there is no "just apply it anyway" path.
  if (!HalClock::isPlausibleEpoch(timestamp)) return ApplyResult::INVALID;

  uint8_t location[MAX_PROGRESS_BYTES] = {};
  size_t locationLen = 0;
  if (!decodeLocation(locationHex, location, locationLen)) return ApplyResult::INVALID;
  if (!isValidLocationLength(relativePath, locationLen)) return ApplyResult::INVALID;

  const std::string fullPath = std::string(booksRoot) + "/" + relativePath;
  if (!Storage.exists(fullPath.c_str())) return ApplyResult::NOT_FOUND;

  // Non-empty: isSupportedBookName() above already established the extension.
  const std::string cachePath = cachePathForBook(fullPath);

  uint32_t deviceEpoch = 0;
  if (ProgressFile::readSavedTime(cachePath, deviceEpoch) && timestamp <= deviceEpoch) {
    LOG_DBG(TAG, "Skipping %s: incoming %lu is not newer than %lu", relativePath.c_str(),
            static_cast<unsigned long>(timestamp), static_cast<unsigned long>(deviceEpoch));
    return ApplyResult::SKIPPED_OLDER;
  }

  // A book that was never opened has no cache directory yet; the reader creates
  // it on open, so create it here too rather than dropping the position.
  if (!Storage.ensureDirectoryExists(cachePath.c_str())) {
    LOG_ERR(TAG, "Could not create cache dir for %s", relativePath.c_str());
    return ApplyResult::WRITE_FAILED;
  }
  if (!ProgressFile::writeAtomicAt(cachePath, location, locationLen, timestamp)) {
    return ApplyResult::WRITE_FAILED;
  }
  LOG_DBG(TAG, "Applied progress for %s (%u bytes @ %lu)", relativePath.c_str(), static_cast<unsigned>(locationLen),
          static_cast<unsigned long>(timestamp));
  return ApplyResult::APPLIED;
}

}  // namespace BookProgressSync
