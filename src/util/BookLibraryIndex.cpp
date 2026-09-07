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

#include "ProgressMapper.h"
#include "TaskWatchdog.h"

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
};

// progress.bin is opaque and format-specific; these two readers mirror exactly
// what the reader activities write (EpubReaderActivity::loadProgress and
// XtcReaderActivity::loadProgress). Nothing else about the file is interpreted
// here -- the position it yields is handed to ProgressMapper.
bool readEpubProgress(const std::string& cachePath, CrossPointPosition& pos) {
  HalFile f;
  if (!Storage.openFileForRead(TAG, cachePath + "/progress.bin", f)) return false;
  uint8_t data[10] = {};
  const int read = f.read(data, sizeof(data));
  if (read != 4 && read != 6 && read != 10) return false;

  pos.spineIndex = data[0] | (data[1] << 8);
  int page = data[2] | (data[3] << 8);
  // The reader treats the all-ones page as a stale "last page" sentinel and
  // ignores it; mirroring that keeps a book from reading as 100% here while the
  // reader would reopen it at page 0.
  if (page == UINT16_MAX) page = 0;
  pos.pageNumber = page;
  // The 4-byte form predates the chapter page count. Without it there is no
  // intra-chapter fraction, so progress resolves to the chapter boundary.
  pos.totalPages = (read >= 6) ? (data[4] | (data[5] << 8)) : 0;
  return true;
}

bool readXtcProgressPage(const std::string& cachePath, uint32_t& page) {
  HalFile f;
  if (!Storage.openFileForRead(TAG, cachePath + "/progress.bin", f)) return false;
  uint8_t data[4] = {};
  if (f.read(data, sizeof(data)) != static_cast<int>(sizeof(data))) return false;
  page = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  return true;
}

BookInfo describeBook(const std::string& path, const std::string& fileName) {
  BookInfo info;

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
        if (readEpubProgress(epub->getCachePath(), pos)) {
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
      if (readXtcProgressPage(xtc.getCachePath(), page)) {
        info.percent = static_cast<float>(xtc.calculateProgress(page)) / 100.0f;
      }
    }
  }
  // .txt/.md carry no embedded metadata, and their progress.bin holds a page
  // index whose page count depends on the current font/margin/viewport, none of
  // which is recorded next to it. There is no percentage on disk to report, so
  // they list with the filename as title and percent 0.

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
  // "lastRead" is deliberately absent. Nothing on the device records when a book
  // was last opened: progress.bin carries a position and no time, SdFat has no
  // date-time callback installed here (so file timestamps are not real), and the
  // RTC exposes only hour/minute. Emitting an invented value would be worse than
  // omitting the field, which the protocol allows.
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
