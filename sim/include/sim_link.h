#pragma once
// Simulator: the memory the window process (sim/src/host.cpp) shares with the firmware process it runs. The firmware runs
// in a child process so that ESP.restart() and deep sleep start it afresh, as on the device, while the window stays open.
#include <stdint.h>

namespace sim {

constexpr int kExitRestart = 40;  // ESP.restart()
constexpr int kExitSleep = 41;    // deep sleep until the power button

struct Shared {
  int32_t x3;           // panel: X3 (792x528) or X4 (800x480); set by the host
  int32_t slow;         // simulate the panel's refresh time
  int32_t resetReason;  // esp_reset_reason() of the next run
  int32_t battery;      // percent, the host's +/- keys change it
  int32_t wifiDown;     // the host's W key: Wi-Fi out of reach
  volatile uint32_t buttons;   // InputManager bitmask, written by the host
  volatile uint32_t frameSeq;  // bumped by the firmware after each refresh
  int32_t fbWidth, fbHeight, fbStride;  // landscape panel framebuffer, 1 bit per pixel, 1 = white
  int32_t rtcSize;
  uint8_t rtc[16 * 1024];  // RTC_NOINIT_ATTR variables, kept across restarts
  uint8_t fb[100 * 528];
};

Shared& shared();
}  // namespace sim
