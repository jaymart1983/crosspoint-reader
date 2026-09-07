#include "BookLibraryIndex.h"

#include <ArduinoJson.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "BookProgressSync.h"
#include "ProgressMapper.h"
#include "TaskWatchdog.h"
#include "activities/reader/ProgressFile.h"

namespace {

constexpr const char* TAG = "BLIB";
constexpr const char* CACHE_ROOT = "/.crosspoint";
// SdFat long names are up to 255 characters, which UTF-8 can widen well past 256
// bytes; a truncated name would list the wrong file, so keep the same headroom
// NextBookFinder uses.
constexpr size_t NAME_BUFFER_SIZE = 512;

// /Books/a/b/c/book.epub is the deepest layout walked. The listing mirrors a
// shelf, not an arbitrary SD card, and every extra level is another full
// directory pass; anything deeper is logged rather than silently dropped.
constexpr size_t MAX_DIR_DEPTH = 4;

// Formats ReaderActivity can open. Deliberately the same set as
// NextBookFinder's: a "book" is something the reader can actually open, so
// covers and stray .bmp files stay out of the listing.
bool isSupportedBookFile(const std::string_view name) {
  return FsHelpers::hasEpubExtension(name) || FsHelpers::hasXtcExtension(name) || FsHelpers::hasTxtExtension(name) ||
         FsHelpers::hasMarkdownExtension(name);
}

struct BookEntry {
  std::string name;
  uint32_t size = 0;
};

struct BookInfo {
  std::string title;
  std::string author;
  float percent = 0.0f;
  bool fromMetadataCache = false;
  // The saved position exactly as progress.bin holds it, hex-encoded. Empty when
  // the book has never been opened, or its progress file is unreadable.
  std::string location;
  // When that position was saved. Absent for a book saved before the device kept
  // timestamps, or saved while it had no working clock -- which a client must
  // read as "unknown", never as epoch 0.
  uint32_t timestamp = 0;
  bool hasTimestamp = false;
};

// progress.bin is opaque and format-specific; these two parsers mirror exactly
// what the reader activities write (EpubReaderActivity::loadProgress and
// XtcReaderActivity::loadProgress). They exist only to derive `percent` for
// display -- the bytes themselves are what the listing actually reports, in
// `location`, and they are copied out verbatim without being interpreted.
bool parseEpubProgress(const uint8_t* data, const size_t len, CrossPointPosition& pos) {
  if (len != 4 && len != 6 && len != 10) return false;

  pos.spineIndex = data[0] | (data[1] << 8);
  int page = data[2] | (data[3] << 8);
  // The reader treats the all-ones page as a stale "last page" sentinel and
  // ignores it; mirroring that keeps a book from reading as 100% here while the
  // reader would reopen it at page 0.
  if (page == UINT16_MAX) page = 0;
  pos.pageNumber = page;
  // The 4-byte form predates the chapter page count. Without it there is no
  // intra-chapter fraction, so progress resolves to the chapter boundary.
  pos.totalPages = (len >= 6) ? (data[4] | (data[5] << 8)) : 0;
  return true;
}

bool parseXtcProgressPage(const uint8_t* data, const size_t len, uint32_t& page) {
  if (len != 4) return false;
  page = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  return true;
}

BookInfo describeBook(const std::string& path, const std::string& fileName) {
  BookInfo info;

  // The position first, and independently of metadata: it is the field the sync
  // path actually needs, and it is readable for every format the reader can open
  // -- including .txt/.md, whose percentage is not recoverable.
  uint8_t location[BookProgressSync::MAX_PROGRESS_BYTES] = {};
  size_t locationLen = 0;
  const std::string cachePath = BookProgressSync::cachePathForBook(path);
  const bool hasLocation = BookProgressSync::readProgressBlob(cachePath, location, locationLen);
  if (hasLocation) {
    info.location = BookProgressSync::encodeLocation(location, locationLen);
    info.hasTimestamp = ProgressFile::readSavedTime(cachePath, info.timestamp);
  }

  if (FsHelpers::hasEpubExtension(fileName)) {
    // The per-book metadata cache is the only affordable source of title/author:
    // buildIfMissing = false means an unopened book yields blanks rather than a
    // full indexing pass, and skipLoadingCss = true keeps the load off the CSS
    // rebuild path entirely.
    auto owned = makeUniqueNoThrow<Epub>(path, CACHE_ROOT);
    if (!owned) {
      LOG_ERR(TAG, "OOM loading metadata for %s", path.c_str());
    } else {
      const std::shared_ptr<Epub> epub(std::move(owned));
      if (epub->load(false, true)) {
        info.title = epub->getTitle();
        info.author = epub->getAuthor();
        info.fromMetadataCache = true;
        CrossPointPosition pos{};
        if (hasLocation && parseEpubProgress(location, locationLen, pos)) {
          info.percent = ProgressMapper::toPercentage(epub, pos);
        }
      }
    }
  } else if (FsHelpers::hasXtcExtension(fileName)) {
    // XTC carries its metadata in the file header, so this is a cheap read and
    // works even for a book that was never opened.
    Xtc xtc(path, CACHE_ROOT);
    if (xtc.load()) {
      info.title = xtc.getTitle();
      info.author = xtc.getAuthor();
      info.fromMetadataCache = true;
      uint32_t page = 0;
      if (hasLocation && parseXtcProgressPage(location, locationLen, page)) {
        info.percent = static_cast<float>(xtc.calculateProgress(page)) / 100.0f;
      }
    }
  }
  // .txt/.md carry no embedded metadata, and their progress.bin holds a page
  // index whose page count depends on the current font/margin/viewport, none of
  // which is recorded next to it. There is no percentage on disk to report, so
  // they list with the filename as title and percent 0 -- but `location` and
  // `timestamp` above are still exact, which is precisely why syncing on the
  // stored position rather than a percentage covers these books at all.

  if (info.title.empty()) {
    // Never omit a book because its metadata is missing: the filename is always
    // something the phone can render.
    info.title = fileName;
    info.fromMetadataCache = false;
  }
  info.percent = std::clamp(info.percent, 0.0f, 1.0f);
  return info;
}

// Reads one directory into books + subdirectories, then closes it. The listing
// deliberately does not hold a directory handle open while loading a book's
// metadata cache, which opens files of its own.
bool scanDirectory(const std::string& dirPath, std::vector<BookEntry>& books, std::vector<std::string>& subdirs) {
  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }
  dir.rewindDirectory();

  const auto nameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!nameBuffer) {
    LOG_ERR(TAG, "OOM: %d bytes", static_cast<int>(NAME_BUFFER_SIZE));
    dir.close();
    return false;
  }

  for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    entry.getName(nameBuffer.get(), NAME_BUFFER_SIZE);
    const bool isDir = entry.isDirectory();
    const auto size = static_cast<uint32_t>(isDir ? 0 : entry.fileSize());
    entry.close();

    // Dot entries are the reader's own caches and host-OS litter, never shelf content.
    if (nameBuffer[0] == '\0' || nameBuffer[0] == '.') continue;
    if (isDir) {
      subdirs.emplace_back(nameBuffer.get());
      continue;
    }
    if (!isSupportedBookFile(nameBuffer.get())) continue;
    books.push_back(BookEntry{std::string(nameBuffer.get()), size});
  }
  dir.close();

  // Same ordering the file browser shows, so the phone's list matches the device's.
  std::sort(books.begin(), books.end(),
            [](const BookEntry& a, const BookEntry& b) { return FsHelpers::naturalLess(a.name, b.name); });
  std::sort(subdirs.begin(), subdirs.end(),
            [](const std::string& a, const std::string& b) { return FsHelpers::naturalLess(a, b); });
  return true;
}

bool writeEntry(HalFile& out, bool& first, const std::string& relPath, const uint32_t size, const BookInfo& info) {
  // One book per document: this is the whole reason the listing fits in RAM.
  JsonDocument doc;
  doc["filename"] = relPath;
  doc["size"] = size;
  doc["title"] = info.title;
  doc["author"] = info.author;
  // Trimmed to four decimals: 0.01% is far finer than any progress bar, and the
  // shorter number keeps a large shelf's document smaller on the wire.
  doc["percent"] = std::round(info.percent * 10000.0f) / 10000.0f;
  // `location` is the field to sync on; `percent` is for display only. It is the
  // saved position byte for byte, so a client can hand it straight back in a
  // `progress` upload and the book reopens exactly where it was -- no percentage
  // is ever converted into a position.
  if (!info.location.empty()) doc["location"] = info.location;
  // Omitted, not zeroed, when the save predates timestamping or happened while
  // the device had no clock. An absent `timestamp` means "unknown", which the
  // conflict rule treats as older than any real one.
  if (info.hasTimestamp) doc["timestamp"] = info.timestamp;
  // "lastRead" is still deliberately absent. It was specified as optional and
  // vague ("when was this book last read"); `timestamp` answers the precise
  // question the sync path asks -- when the position in `location` was written --
  // so clients should read that instead.
  String json;
  serializeJson(doc, json);

  if (!first && out.print(",") != 1) return false;
  first = false;
  return out.print(json) == json.length();
}

}  // namespace

bool BookLibraryIndex::build(const char* booksRoot, const char* outPath, Stats* stats) {
  if (!booksRoot || !outPath) return false;

  HalFile out;
  if (!Storage.openFileForWrite(TAG, outPath, out)) {
    LOG_ERR(TAG, "Could not open library index for write: %s", outPath);
    return false;
  }

  Stats collected;
  bool ok = out.print("[") == 1;
  bool first = true;

  // Explicit worklist rather than recursion: one directory handle is open at a
  // time, and the traversal cannot blow the task stack on a deep tree.
  std::vector<std::string> pendingDirs;
  pendingDirs.emplace_back();  // the books root itself

  while (ok && !pendingDirs.empty()) {
    const std::string rel = std::move(pendingDirs.back());
    pendingDirs.pop_back();
    const std::string dirPath = rel.empty() ? std::string(booksRoot) : std::string(booksRoot) + "/" + rel;

    std::vector<BookEntry> books;
    std::vector<std::string> subdirs;
    if (!scanDirectory(dirPath, books, subdirs)) {
      // A folder that cannot be read costs its own books, not the whole listing.
      LOG_ERR(TAG, "Could not read folder: %s", dirPath.c_str());
      continue;
    }

    const size_t depth = rel.empty() ? 0 : static_cast<size_t>(std::count(rel.begin(), rel.end(), '/')) + 1;
    if (!subdirs.empty()) {
      if (depth + 1 <= MAX_DIR_DEPTH) {
        // Pushed in reverse so the stack pops them in sorted order.
        for (auto it = subdirs.rbegin(); it != subdirs.rend(); ++it) {
          pendingDirs.push_back(rel.empty() ? *it : rel + "/" + *it);
        }
      } else {
        LOG_ERR(TAG, "Not descending past depth %d: %s (%d sub-folders skipped)", static_cast<int>(MAX_DIR_DEPTH),
                dirPath.c_str(), static_cast<int>(subdirs.size()));
      }
    }

    for (const auto& book : books) {
      // Each book is an SD-bound metadata load; a large shelf would otherwise
      // outlast the watchdog window.
      resetTaskWatchdogIfSubscribed();
      const std::string relPath = rel.empty() ? book.name : rel + "/" + book.name;
      const BookInfo info = describeBook(dirPath + "/" + book.name, book.name);
      if (!writeEntry(out, first, relPath, book.size, info)) {
        LOG_ERR(TAG, "Short write building library index at %s", relPath.c_str());
        ok = false;
        break;
      }
      collected.books++;
      if (info.fromMetadataCache) collected.withMetadata++;
      if (info.percent > 0.0f) collected.withProgress++;
    }
  }

  if (ok) ok = out.print("]") == 1;
  out.flush();
  out.close();

  if (!ok) {
    Storage.remove(outPath);
    return false;
  }

  LOG_DBG(TAG, "Library index: %u books (%u with metadata, %u in progress)", static_cast<unsigned>(collected.books),
          static_cast<unsigned>(collected.withMetadata), static_cast<unsigned>(collected.withProgress));
  if (stats) *stats = collected;
  return true;
}
