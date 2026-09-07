#pragma once

#include <Arduino.h>
#include <Rtc.h>

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  mutable Rtc _sdkRtc;
  mutable uint8_t _cachedHour = 0;
  mutable uint8_t _cachedMinute = 0;
  // UTC seconds at _lastPollMs, or 0 when the last RTC read produced a date
  // outside the representable range. Never extrapolated on its own -- see
  // getEpoch(), which adds the elapsed millis().
  mutable uint32_t _cachedEpoch = 0;
  mutable bool _hasCachedTime = false;
  // millis() of the last RTC read attempt (throttles I2C traffic) and of the
  // last one that actually produced a time (the reference _cachedEpoch is
  // carried forward from). They diverge only while the RTC is unreadable.
  mutable unsigned long _lastPollMs = 0;
  mutable unsigned long _lastSyncMs = 0;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

  // Reads the RTC at most once per CLOCK_POLL_MS and refreshes the cache.
  // A failed read (I2C error, or an oscillator the RTC reports as stopped --
  // i.e. the clock was never set or lost its backup power) leaves the cache
  // untouched, so "no time" stays "no time" rather than becoming a made-up one.
  void refreshCache() const;

 public:
  // A clock this device has never had set reads as 2000-01-01 at best, so
  // anything before 2020 is treated as "not a real wall clock". The upper bound
  // keeps a garbled I2C read from becoming a timestamp no later save can beat.
  static constexpr uint32_t MIN_VALID_EPOCH = 1577836800UL;  // 2020-01-01T00:00:00Z
  static constexpr uint32_t MAX_VALID_EPOCH = 4102444800UL;  // 2100-01-01T00:00:00Z

  static constexpr bool isPlausibleEpoch(const uint32_t epochUtc) {
    return epochUtc >= MIN_VALID_EPOCH && epochUtc < MAX_VALID_EPOCH;
  }

  // Call after BoardConfig has selected the active device.
  void begin();

  // True if an RTC is present on this device
  bool isAvailable() const { return _available; }

  // Get current hour (0-23) and minute (0-59).
  // Returns false if RTC is not available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Current UTC time as seconds since the Unix epoch.
  //
  // Returns false -- meaning "this device does not know what time it is" -- when
  // there is no RTC, when the RTC reports its oscillator stopped (never set, or
  // backup power lost), or when the date it holds is not plausible wall-clock
  // time. Callers must treat that as unknown and must never substitute 0: an
  // epoch of 0 would compare as a real, very old timestamp.
  //
  // The reader stamps saved progress with this, and the BLE sync path compares
  // against it, so it is deliberately stricter than getTime(): the status bar
  // may keep showing a clock whose date is nonsense, but nothing is ever
  // timestamped from one.
  bool getEpoch(uint32_t& epochUtc) const;

  // Set the RTC from a UTC epoch. Rejects implausible values (see
  // isPlausibleEpoch) rather than writing them. Returns true if the RTC took it.
  //
  // This is how the device learns the time: there is no NTP on a build without
  // the network stack, so the phone sets it over BLE (`set_time`).
  bool setEpoch(uint32_t epochUtc);

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // utcOffsetQuarterHoursBiased: biased quarter-hour offset (48 = UTC+0, 0 = UTC-12, 104 = UTC+14).
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;

  // Sync the RTC from an NTP server. Requires WiFi to be connected.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the RTC was successfully updated.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();
};
