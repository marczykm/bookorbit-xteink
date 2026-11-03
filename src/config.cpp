#include "config.h"

#include <ArduinoJson.h>
#include <SDCardManager.h>

bool Config::load(String& error) {
  if (!SdMan.exists("/bookorbit.json")) {
    error = "Missing /bookorbit.json on the SD card";
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, SdMan.readFile("/bookorbit.json"))) {
    error = "/bookorbit.json is not valid JSON";
    return false;
  }
  ssid = doc["ssid"] | "";
  wifiPassword = doc["wifiPassword"] | "";
  server = doc["server"] | "";
  username = doc["username"] | "";
  password = doc["password"] | "";
  insecureTls = doc["insecureTls"] | false;
  timezone = doc["timezone"] | "";
  while (server.endsWith("/")) server.remove(server.length() - 1);
  if (ssid.isEmpty() || server.isEmpty() || username.isEmpty()) {
    error = "/bookorbit.json needs ssid, server and username";
    return false;
  }
  return true;
}
