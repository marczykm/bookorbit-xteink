#pragma once
// Touch on boards with a touchscreen (the X4 Pro). The screens register what they draw as tap targets: a tap selects the
// target's value and presses a button (OK by default), so every screen works by touch without its own handling.
// Gestures arrive from the button task through a small queue. Without a touchscreen this all compiles to nothing.
#include <InputManager.h>
#include <stdint.h>

#ifndef FREEINK_CAP_TOUCH
#define FREEINK_CAP_TOUCH 0
#endif

namespace touch {
constexpr uint8_t kNoButton = 0xFF;

enum class Gesture : uint8_t { Tap, SwipeLeft, SwipeRight, SwipeUp, SwipeDown, Home, HomeLong };
struct Event {
  Gesture gesture;
  int16_t x, y;  // portrait pixels (taps)
};

#if FREEINK_CAP_TOUCH
// Forgets all targets; called when a screen is drawn afresh.
void clear();
// A tap inside the rectangle sets *sel = value (when sel is given) and then presses `button` (unless kNoButton).
// Later targets lie on top of earlier ones (an overlay box over its screen).
void target(int x, int y, int w, int h, int* sel, int value, uint8_t button = InputManager::BTN_CONFIRM);
// The same with a function that takes the value (for a selection kept in more than one variable, like a key's row and
// column).
void targetFn(int x, int y, int w, int h, void (*select)(int), int value, uint8_t button = InputManager::BTN_CONFIRM);
// A tap that just presses `button`.
inline void press(int x, int y, int w, int h, uint8_t button) { target(x, y, w, h, nullptr, 0, button); }
// The tap at (x, y): selects the topmost target there and returns the button it presses (kNoButton for none), or false
// when there is no target there.
bool tap(int x, int y, uint8_t& button);

// Button task: turns the touch controller's tap, swipe and Home-key events into queued gestures; true when it queued one.
bool capture(const InputManager& input);
bool pop(Event& e);  // main loop: the next queued gesture
void inject(const Event& e);  // test hook: queues a gesture as if it was touched
#else
inline void clear() {}
inline void target(int, int, int, int, int*, int, uint8_t = 0) {}
inline void targetFn(int, int, int, int, void (*)(int), int, uint8_t = 0) {}
inline void press(int, int, int, int, uint8_t) {}
inline bool tap(int, int, uint8_t&) { return false; }
inline bool capture(const InputManager&) { return false; }
inline bool pop(Event&) { return false; }
inline void inject(const Event&) {}
#endif
}  // namespace touch
