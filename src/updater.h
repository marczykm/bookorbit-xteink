#pragma once
// Firmware update from the project's GitHub releases: the app image of the latest release (bookorbit-x3-x4-<version>.bin,
// the same file that installs it over USB).
#include <Arduino.h>

namespace updater {
struct Info {
  char version[24] = "";  // e.g. "0.0.2"
  char url[320] = "";     // the release asset (GitHub redirects it to its CDN)
  size_t size = 0;
};

const char* currentVersion();
// Asks GitHub for the latest release: 1 = a newer one is in `info`, 0 = this firmware is current, -1 = error.
int check(Info& info, String& error);
// Downloads the image into the spare app slot and makes it the next boot; `progress` gets 0-100. The caller restarts.
bool install(const Info& info, void (*progress)(int percent), String& error);
}  // namespace updater
