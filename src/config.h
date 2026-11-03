#pragma once
#include <Arduino.h>

// Settings read from /bookorbit.json on the SD card (see config.example.json).
struct Config {
  bool insecureTls = false;  // accept any HTTPS certificate (self-signed servers)
  String timezone;  // POSIX TZ string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"; empty = UTC (only used for the companion's bedtime pose)
  String ssid, wifiPassword, server, username, password;
  bool load(String& error);
};
