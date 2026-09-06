#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

struct BleTrustedHost {
  std::string hostId;
  std::string name;
  std::string secret;  // Plaintext in memory; obfuscated with hardware key on disk.
};

/**
 * Singleton storing BLE trusted-host credentials on the SD card.
 *
 * Ported from the standalone JsonSettingsIO implementation onto
 * PersistableStore, which replaced it in 63eda54 ("Migrate Settings/State onto
 * PersistableStore"). The base now supplies the singleton, saveToFile(),
 * loadFromFile(), locking, and the legacy-shape resave hook, so this class only
 * has to describe its own JSON.
 *
 * The secret is stored under "password_obf" so it goes through the shared
 * extractPassword()/obfuscateToBase64() path used by every other store rather
 * than growing a second obfuscation scheme.
 */
class BleTrustedHostStore : public PersistableStore<BleTrustedHostStore> {
 private:
  std::vector<BleTrustedHost> hosts;

  static constexpr size_t MAX_HOSTS = 1;

  BleTrustedHostStore() = default;

  friend class PersistableStore<BleTrustedHostStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/ble_trusted_hosts.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool addOrReplaceHost(const BleTrustedHost& host);
  const BleTrustedHost* findHost(const std::string& hostId) const;
  bool hasHosts() const { return !hosts.empty(); }
  bool clearAll();

  const std::vector<BleTrustedHost>& getHosts() const { return hosts; }
};

#define BLE_TRUSTED_HOSTS BleTrustedHostStore::getInstance()
