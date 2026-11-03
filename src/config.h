#pragma once
#include <Arduino.h>

// Settings read from /bookorbit.json on the SD card (see config.example.json).
struct Config {
  bool insecureTls = false;  // accept any HTTPS certificate (self-signed servers)
  String ssid, wifiPassword, server, username, password;
  bool load(String& error);
};
