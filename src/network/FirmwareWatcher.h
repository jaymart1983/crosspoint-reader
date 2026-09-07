#pragma once

#include <HalStorage.h>
#include <mbedtls/sha256.h>

#include <cstddef>
#include <cstdint>
#include <string>

// Watches the firmware drop folder (see FirmwareStaging.h) and offers an update
// when what is there checks out.
//
// WHY A WATCHER RATHER THAN A FLOW. Firmware used to arrive through an
// interactive BLE session: the phone pushed an image, the reader validated it
// mid-transfer and threw a Yes/No prompt on screen while the radio was still
// live. That put a reboot-the-device prompt on the far end of a wireless link,
// and it only worked at all while one particular screen was open. Now a drop is
// just a file. The app writes it, or a person writes it with the card mounted
// over USB, and the two routes are the same route.
//
// COST. Hashing a 3 MB image off SD is seconds of work, so it is not done on a
// timer -- it is done once, in HASH_CHUNK_BYTES bites spread across main-loop
// ticks so no single tick is long, and only when the folder's contents have
// actually changed since the last look. An idle poll is two Storage::exists()
// calls every POLL_INTERVAL_MS and nothing else.
class FirmwareWatcher {
 public:
  static FirmwareWatcher& getInstance();

  // Called once per main loop. Cheap unless there is something new to hash.
  // Never called while USB Drive owns the raw card -- main.cpp returns before
  // this on that path, because there is no filesystem to look at.
  void tick();

  // A staged image is present and its bytes hash to what the companion file
  // says. The caller is expected to put the confirmation prompt on screen.
  bool updateReady() const { return phase_ == Phase::READY; }

  // Stop offering the current image. Called when the user declines, and when the
  // prompt has been handed to the update activity so it is not handed over twice.
  // The offer comes back if the file on the card changes, or at the next boot --
  // declining an update does not delete anybody's file.
  void standDown();

 private:
  enum class Phase : uint8_t {
    IDLE,      // nothing staged, or nothing new since the last look
    HASHING,   // reading the image a chunk per tick
    READY,     // hash matched; waiting for the prompt
    DECLINED,  // hash matched but the user said no, or the hash did not match
  };

  FirmwareWatcher() = default;

  void beginHash();
  void finishHash();
  void abandon(const char* reason);

  Phase phase_ = Phase::IDLE;
  unsigned long lastPollMs_ = 0;
  // Size of the staged image the current verdict applies to. A file of a
  // different size is a different drop, so it is looked at again even if the last
  // one was rejected.
  size_t verdictSize_ = 0;

  // Hashing state, live only in Phase::HASHING.
  std::string expectedHash_;
  HalFile image_;
  size_t hashedBytes_ = 0;
  size_t imageSize_ = 0;
  mbedtls_sha256_context sha_{};
  bool shaActive_ = false;
};

#define FIRMWARE_WATCHER FirmwareWatcher::getInstance()
