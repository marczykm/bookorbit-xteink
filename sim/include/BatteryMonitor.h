#pragma once
// Simulator: the battery level is set with + and - in the simulator window.
#include <stdint.h>

class BatteryMonitor {
 public:
  uint16_t readPercentage() const;
};
