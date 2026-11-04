#pragma once
// Simulator: the buttons, read from the simulator window's keyboard, and (in X4 Pro mode) the touchscreen and Home key
// from its mouse and H key.
#include <stdint.h>

// The touch code is always built into the simulator; touch events only come in X4 Pro mode (--x4pro).
#define FREEINK_CAP_TOUCH 1

class InputManager {
 public:
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;

  void begin() {}
  void update();
  bool isPressed(uint8_t b) const { return current_ & (1u << b); }
  bool isPowerButtonPhysicallyPressed() const;
  bool wasPressed(uint8_t b) const { return pressed_ & (1u << b); }
  bool wasAnyPressed() const { return pressed_ != 0; }
  bool wasReleased(uint8_t b) const { return released_ & (1u << b); }
  bool wasAnyReleased() const { return released_ != 0; }
  unsigned long getHeldTime() const;

  // Touch, like the FreeInk SDK: positions normalised to 0..1 in the panel's native (landscape) frame; each event is
  // reported by one update().
  bool hasTouch() const;
  bool wasTouchTap(float& nx, float& ny) const;
  bool wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const;
  bool wasHomeKeyTapped() const { return event_ == 3; }
  bool wasHomeKeyLongPressed() const { return event_ == 4; }

 private:
  uint32_t current_ = 0, pressed_ = 0, released_ = 0;
  unsigned long pressStart_ = 0;
  int event_ = 0;  // this update's touch event: 0 none, 1 tap, 2 swipe, 3 Home tap, 4 Home long press
  float ev_[4] = {};
};
