#pragma once

#include <cstdint>

// Builds the on-device library listing that the BLE `library` download serves:
// one JSON array describing every book under the books root, with the reading
// progress the device already has on disk.
//
// The document is written straight to SD, one book at a time, and never exists
// as a whole in RAM -- a shelf of a few hundred books would otherwise be tens of
// kilobytes of String at exactly the moment a reading session has the least heap
// to spare. The BLE download then streams that file through the ordinary
// frame/ack path, which also gives the transfer a known size and resumability.
namespace BookLibraryIndex {

struct Stats {
  uint32_t books = 0;         // entries written
  uint32_t withMetadata = 0;  // entries whose title came from a cached book metadata record
  uint32_t withProgress = 0;  // entries with a non-zero percent
};

// Writes the JSON array for `booksRoot` to `outPath`, replacing anything already
// there. Returns false (and removes the partial file) if the document could not
// be written in full; an individual unreadable folder or book is logged and
// skipped over, never dropped silently and never fatal.
//
// Blocking and SD-bound: it opens each book's metadata cache in turn, so expect
// tens of milliseconds per book. Callers on the UI task should show something
// first. The task watchdog is fed between books.
bool build(const char* booksRoot, const char* outPath, Stats* stats = nullptr);

}  // namespace BookLibraryIndex
