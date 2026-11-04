#pragma once
// Simulator: the memory the window process (sim/src/host.cpp) shares with the firmware process it runs. The firmware runs
// in a child process so that ESP.restart() and deep sleep start it afresh, as on the device, while the window stays open.
#include <stdint.h>

namespace sim {

constexpr int kExitRestart = 40;  // ESP.restart()
constexpr int kExitSleep = 41;    // deep sleep until the power button

// A touch gesture from the window (X4 Pro mode), in the panel's native frame like the touch controller reports it.
struct TouchRecord {
  enum Kind : int32_t { Tap, Swipe, HomeTap, HomeLong } kind;
  float nx0, ny0, nx1, ny1;  // 0..1; a tap uses the first point
};
constexpr int kTouchRing = 16;

struct Shared {
  int32_t x3;           // panel: X3 (792x528) or X4 (800x480); set by the host
  int32_t x4pro;        // X4 Pro: touchscreen, Home key and frontlight; only the Up/Down side keys and Power
  int32_t slow;         // simulate the panel's refresh time
  int32_t resetReason;  // esp_reset_reason() of the next run
  int32_t battery;      // percent, the host's +/- keys change it
  int32_t wifiDown;     // the host's W key: Wi-Fi out of reach
  volatile uint32_t buttons;   // InputManager bitmask, written by the host
  volatile uint32_t frameSeq;  // bumped by the firmware after each refresh
  TouchRecord touch[kTouchRing];  // written by the host at touchHead, read by the firmware at touchTail
  volatile uint32_t touchHead, touchTail;
  volatile int32_t lightLevel, lightWarm;  // the frontlight as the firmware set it (0-100), for the window to show
  int32_t fbWidth, fbHeight, fbStride;  // landscape panel framebuffer, 1 bit per pixel, 1 = white
  int32_t rtcSize;
  uint8_t rtc[16 * 1024];  // RTC_NOINIT_ATTR variables, kept across restarts
  uint8_t fb[100 * 528];
};

Shared& shared();
}  // namespace sim
