#include "HalClock.h"

#include <Logging.h>
#include <time.h>
#if FREEINK_CAP_NETWORK
#include <WiFi.h>
#include <esp_sntp.h>
#endif

namespace {

// Proleptic Gregorian civil <-> days-since-1970 (Howard Hinnant's algorithms).
//
// Deliberately not mktime()/gmtime(): the RTC holds UTC and this build has no
// network stack, so nothing ever calls configTzTime() and libc's TZ is whatever
// the core left it. These two functions are pure arithmetic and cannot be
// perturbed by a time zone.
int32_t daysFromCivil(int32_t y, const uint32_t m, const uint32_t d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(y - era * 400);                        // [0, 399]
  const uint32_t doy = (153U * (m + (m > 2 ? -3U : 9U)) + 2U) / 5U + d - 1U;         // [0, 365]
  const uint32_t doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;                     // [0, 146096]
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

void civilFromDays(int32_t z, int32_t& y, uint32_t& m, uint32_t& d) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = static_cast<uint32_t>(z - era * 146097);                      // [0, 146096]
  const uint32_t yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;    // [0, 399]
  const uint32_t doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);                   // [0, 365]
  const uint32_t mp = (5U * doy + 2U) / 153U;                                        // [0, 11]
  d = doy - (153U * mp + 2U) / 5U + 1U;                                              // [1, 31]
  m = mp + (mp < 10U ? 3U : -9U);                                                    // [1, 12]
  y = static_cast<int32_t>(yoe) + era * 400 + static_cast<int32_t>(m <= 2U);
}

// 0 means "this date is not a representable UTC epoch", which every caller
// treats as unknown. Nothing downstream may read 0 as a real instant.
uint32_t epochFromDateTime(const Rtc::DateTime& dt) {
  if (dt.year < 1970 || dt.year > 2100 || dt.month < 1 || dt.month > 12 || dt.day < 1 || dt.day > 31 ||
      dt.hour > 23 || dt.minute > 59 || dt.second > 59) {
    return 0;
  }
  const int32_t days = daysFromCivil(static_cast<int32_t>(dt.year), dt.month, dt.day);
  if (days < 0) return 0;
  return static_cast<uint32_t>(days) * 86400UL + static_cast<uint32_t>(dt.hour) * 3600UL +
         static_cast<uint32_t>(dt.minute) * 60UL + dt.second;
}

Rtc::DateTime dateTimeFromEpoch(const uint32_t epochUtc) {
  const int32_t days = static_cast<int32_t>(epochUtc / 86400UL);
  const uint32_t secondOfDay = epochUtc % 86400UL;
  int32_t year = 1970;
  uint32_t month = 1;
  uint32_t day = 1;
  civilFromDays(days, year, month, day);

  Rtc::DateTime dt;
  dt.year = static_cast<uint16_t>(year);
  dt.month = static_cast<uint8_t>(month);
  dt.day = static_cast<uint8_t>(day);
  dt.hour = static_cast<uint8_t>(secondOfDay / 3600UL);
  dt.minute = static_cast<uint8_t>((secondOfDay % 3600UL) / 60UL);
  dt.second = static_cast<uint8_t>(secondOfDay % 60UL);
  // 1970-01-01 was a Thursday, and DateTime::weekday is 0 = Sunday.
  dt.weekday = static_cast<uint8_t>(((days % 7) + 11) % 7);
  return dt;
}

}  // namespace

HalClock halClock;  // Singleton instance

void HalClock::begin() {
  _available = _sdkRtc.begin();
  LOG_INF("CLK", _available ? "SDK RTC found" : "RTC not found");
  seedFromBuildEpoch();
}

void HalClock::seedFromBuildEpoch() {
  if (!_available) return;
  if (!isPlausibleEpoch(BUILD_EPOCH)) {
    // Only reachable in a build where scripts/build_epoch.py did not run. Say so
    // rather than making up a date: an unstamped save is bad, a wrong one worse.
    LOG_ERR("CLK", "No usable build epoch compiled in; clock left as the RTC reports it");
    return;
  }

  uint32_t current = 0;
  if (!getEpoch(current)) {
    // No plausible time: the oscillator is stopped (never set, or backup power
    // lost) or the date registers hold nonsense. Start it at the build epoch.
    if (setEpoch(BUILD_EPOCH)) {
      LOG_INF("CLK", "RTC had no valid time; seeded from the build epoch %lu (behind until the app sets it)",
              static_cast<unsigned long>(BUILD_EPOCH));
    } else {
      LOG_ERR("CLK", "RTC had no valid time and could not be seeded");
    }
    return;
  }

  if (current < BUILD_EPOCH) {
    // A running clock that predates the firmware it is running is wrong, so
    // move it forward to the lower bound. This is the only case where a valid
    // reading is overwritten, and it is always forwards.
    if (setEpoch(BUILD_EPOCH)) {
      LOG_INF("CLK", "RTC read %lu, before the build epoch %lu; advanced to the build epoch",
              static_cast<unsigned long>(current), static_cast<unsigned long>(BUILD_EPOCH));
    } else {
      LOG_ERR("CLK", "RTC read %lu, before the build epoch, but could not be advanced",
              static_cast<unsigned long>(current));
    }
    return;
  }

  // The common case after the first boot: keep what the RTC holds. Only a
  // `set_time` from the app changes it from here, correcting the seed's drift.
  LOG_INF("CLK", "RTC already valid at %lu; left alone", static_cast<unsigned long>(current));
}

void HalClock::refreshCache() const {
  if (!_available) return;
  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) return;

  Rtc::DateTime dt;
  if (!_sdkRtc.now(dt)) {
    // Either an I2C error or the RTC telling us its oscillator stopped, which is
    // how a never-set (or backup-power-lost) clock reports itself. Throttle the
    // retry but keep any cache we already have -- and, crucially, do not
    // manufacture a time here.
    _lastPollMs = now;
    return;
  }
  _cachedHour = dt.hour;
  _cachedMinute = dt.minute;
  _cachedEpoch = epochFromDateTime(dt);
  _lastPollMs = now;
  _lastSyncMs = now;
  _hasCachedTime = true;
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;
  refreshCache();
  if (!_hasCachedTime) return false;
  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

bool HalClock::getEpoch(uint32_t& epochUtc) const {
  if (!_available) return false;
  refreshCache();
  if (!_hasCachedTime || !isPlausibleEpoch(_cachedEpoch)) return false;
  // The RTC is only read every CLOCK_POLL_MS; carry the cached instant forward
  // with millis() so two saves inside one poll window are still ordered.
  const unsigned long elapsedMs = millis() - _lastSyncMs;
  const uint32_t candidate = _cachedEpoch + static_cast<uint32_t>(elapsedMs / 1000UL);
  if (!isPlausibleEpoch(candidate)) return false;
  epochUtc = candidate;
  return true;
}

bool HalClock::setEpoch(const uint32_t epochUtc) {
  if (!_available) return false;
  if (!isPlausibleEpoch(epochUtc)) {
    LOG_ERR("CLK", "Refusing to set RTC to implausible epoch %lu", static_cast<unsigned long>(epochUtc));
    return false;
  }
  const Rtc::DateTime dt = dateTimeFromEpoch(epochUtc);
  if (!_sdkRtc.set(dt)) {
    LOG_ERR("CLK", "RTC write failed");
    return false;
  }
  _cachedHour = dt.hour;
  _cachedMinute = dt.minute;
  _cachedEpoch = epochUtc;
  _lastPollMs = millis();
  _lastSyncMs = _lastPollMs;
  _hasCachedTime = true;
  LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02u:%02u:%02u UTC (epoch %lu)", dt.year, dt.month, dt.day, dt.hour,
          dt.minute, dt.second, static_cast<unsigned long>(epochUtc));
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
  // Clamp against corrupted persisted values so display time can't drift outside [-12:00, +14:00].
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::syncFromNTP() {
  if (!_available) return false;
#if !FREEINK_CAP_NETWORK
  // No network stack on this board: the RTC is set from the manual UTC offset
  // picker only. Kept as a symbol so HalClock has one declaration everywhere.
  LOG_ERR("CLK", "NTP sync unavailable in a build without the network stack");
  return false;
#else

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      Rtc::DateTime dt;
      dt.year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
      dt.month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
      dt.day = static_cast<uint8_t>(timeinfo.tm_mday);
      dt.hour = static_cast<uint8_t>(timeinfo.tm_hour);
      dt.minute = static_cast<uint8_t>(timeinfo.tm_min);
      dt.second = static_cast<uint8_t>(timeinfo.tm_sec);
      dt.weekday = static_cast<uint8_t>(timeinfo.tm_wday);
      if (_sdkRtc.set(dt)) {
        _lastPollMs = 0;  // force a re-read on the next poll
        _cachedHour = dt.hour;
        _cachedMinute = dt.minute;
        _cachedEpoch = epochFromDateTime(dt);
        _lastSyncMs = millis();
        _hasCachedTime = true;
        LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02u:%02u:%02u UTC", dt.year, dt.month, dt.day, dt.hour, dt.minute,
                dt.second);
        return true;
      }
      return false;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
#endif  // FREEINK_CAP_NETWORK
}
