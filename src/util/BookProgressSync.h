#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Exact, lossless reading-position exchange for the BLE `progress` upload and
// the `location` field of the `library` download.
//
// The unit of exchange is the *stored position*, never a percentage. progress.bin
// holds a few bytes whose meaning is private to the reader activity that wrote
// them (EPUB: spine index, page, chapter page count and optionally a visible-text
// offset; XTC and TXT: a page index). A percentage is derived from that for
// display and is lossy in both directions -- turning 42.37% back into a spine and
// page needs the book's pagination, which depends on the current font, margins
// and viewport, so the round trip does not close. So nothing here interprets the
// bytes: they go out as opaque hex and come back byte for byte.
namespace BookProgressSync {

// The longest progress.bin any reader writes (EPUB's 10-byte form). A larger
// file is a format from some future firmware; refuse it rather than truncate.
constexpr size_t MAX_PROGRESS_BYTES = 10;

// Outcome of applying one incoming entry. These names are the wire values in the
// per-entry result document, so keep them stable.
enum class ApplyResult {
  APPLIED,        // written, and stamped with the incoming timestamp
  SKIPPED_OLDER,  // the device's own save is at least as new
  NOT_FOUND,      // no such book under the books root
  UNSUPPORTED,    // a file the reader cannot open, so it has no position format
  INVALID,        // malformed filename, location or timestamp
  WRITE_FAILED,   // the position could not be persisted
};

const char* applyResultName(ApplyResult result);

// True when the reader can open a file with this name -- i.e. it has a saved
// position format at all. Extension only; the file need not exist.
bool isSupportedBookName(const std::string& fileName);

// The reader cache directory for a book file, or "" when the file is not one the
// reader can open. Derived through the same Epub/Xtc/Txt constructors the reader
// uses, so the hash formula stays single-sourced.
std::string cachePathForBook(const std::string& fullPath);

// Reads `<cachePath>/progress.bin` whole. Returns false when it is absent, empty
// or longer than MAX_PROGRESS_BYTES.
bool readProgressBlob(const std::string& cachePath, uint8_t* out, size_t& len);

// Lowercase hex, two characters per byte. This is the `location` encoding: hex
// rather than base64 because a saved position is at most ten bytes (twenty
// characters), it stays readable in a log or a bug report, and it needs no
// padding or alphabet caveats in the protocol document.
std::string encodeLocation(const uint8_t* data, size_t len);

// Inverse of encodeLocation. Rejects odd lengths, non-hex characters, empty
// input and anything longer than MAX_PROGRESS_BYTES.
bool decodeLocation(const std::string& hex, uint8_t* out, size_t& len);

// True when `len` is a position size the reader for `fileName`'s type actually
// writes. Guards against a client inventing a length that a reader would then
// misparse -- for EPUB the byte count selects the format.
bool isValidLocationLength(const std::string& fileName, size_t len);

// Applies one incoming position to the book at `booksRoot/relativePath`.
//
// Conflict rule: the write happens only when `timestamp` is strictly newer than
// the timestamp stored beside the device's own progress.bin. A device timestamp
// that is unknown -- no sidecar at all, which is every book saved by firmware
// older than this feature or saved while the device had no clock -- counts as
// the oldest possible, so any real incoming timestamp wins. Equal timestamps do
// not apply: an echo of what the device already sent must be a no-op.
ApplyResult applyProgress(const char* booksRoot, const std::string& relativePath, const std::string& locationHex,
                          uint32_t timestamp);

}  // namespace BookProgressSync
