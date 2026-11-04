#pragma once
// Simulator: the X4 Pro frontlight. Present only in X4 Pro mode (--x4pro); the window shows its level and warmth.
#include <stdint.h>

class FrontlightManager {
 public:
  void begin() {}
  bool present() const;
  bool hasColorTemperature() const { return present(); }
  void setBrightness(uint8_t percent);
  void setColorTemperature(uint8_t warmPercent);
  void prepareForDeepSleep();
  uint8_t brightness() const { return level_; }
  uint8_t colorTemperature() const { return warm_; }

 private:
  uint8_t level_ = 0, warm_ = 50;
};
