#include "BleTrustedHostStore.h"

#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>

void BleTrustedHostStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["hosts"].to<JsonArray>();
  for (const auto& host : hosts) {
    JsonObject obj = arr.add<JsonObject>();
    obj["host_id"] = host.hostId;
    obj["name"] = host.name;
    obj["password_obf"] = obfuscation::obfuscateToBase64(host.secret);
  }
}

bool BleTrustedHostStore::fromJson(JsonVariantConst doc) {
  // Tolerate a missing/invalid 'hosts' key (treat as empty); only a JSON parse
  // error is fatal. A null JsonArray iterates zero times.
  hosts.clear();
  JsonArrayConst arr = doc["hosts"].as<JsonArrayConst>();
  bool needsResave = false;

  for (JsonObjectConst obj : arr) {
    if (hosts.size() >= MAX_HOSTS) break;
    BleTrustedHost host;
    host.hostId = obj["host_id"] | "";
    host.name = obj["name"] | "";
    host.secret = extractPassword(obj, needsResave);
    if (host.hostId.empty() || host.secret.empty()) continue;
    hosts.push_back(std::move(host));
  }

  LOG_DBG("BTH", "Loaded %zu BLE trusted host(s)", hosts.size());

  if (needsResave) {
    LOG_DBG("BTH", "Resaving BLE trusted hosts with obfuscated secrets");
    requestResave();
  }

  return true;
}

bool BleTrustedHostStore::addOrReplaceHost(const BleTrustedHost& host) {
  if (host.hostId.empty() || host.secret.empty()) return false;

  const auto oldHosts = hosts;
  hosts.clear();
  hosts.push_back(host);
  if (!saveToFile()) {
    hosts = oldHosts;
    return false;
  }
  LOG_DBG("BTH", "Saved BLE trusted host");
  return true;
}

const BleTrustedHost* BleTrustedHostStore::findHost(const std::string& hostId) const {
  const auto host =
      std::find_if(hosts.begin(), hosts.end(), [&hostId](const BleTrustedHost& item) { return item.hostId == hostId; });
  return host == hosts.end() ? nullptr : &*host;
}

bool BleTrustedHostStore::clearAll() {
  const auto oldHosts = hosts;
  hosts.clear();
  if (!saveToFile()) {
    hosts = oldHosts;
    return false;
  }
  LOG_DBG("BTH", "Cleared BLE trusted hosts");
  return true;
}
