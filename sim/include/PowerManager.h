#pragma once
// Simulator: deep sleep ends the firmware process; the window wakes it again on the power button.
namespace freeink {
class PowerManager {
 public:
  [[noreturn]] static void deepSleepUntilPowerButton();
};
}  // namespace freeink
