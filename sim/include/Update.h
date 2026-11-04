#pragma once
// Simulator: there is no flash to install an update into; Update.begin() fails, so Settings > Update shows its error path.
#include "Arduino.h"

class UpdateClass {
 public:
  bool begin(size_t) { return false; }
  size_t write(const uint8_t*, size_t) { return 0; }
  bool end(bool = false) { return false; }
  void abort() {}
  const char* errorString() const { return "Not available in the simulator"; }
  bool hasError() const { return true; }
};
extern UpdateClass Update;
