#pragma once
// Simulator: the buttons, read from the simulator window's keyboard.
#include <stdint.h>

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

 private:
  uint32_t current_ = 0, pressed_ = 0, released_ = 0;
  unsigned long pressStart_ = 0;
};
