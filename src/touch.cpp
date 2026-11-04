#include "touch.h"

#if FREEINK_CAP_TOUCH
#include <math.h>

#include "gfx.h"

namespace touch {
namespace {
struct Target {
  int16_t x, y, w, h;
  int* sel;
  void (*select)(int);
  int value;
  uint8_t button;
};
constexpr int kMaxTargets = 72;  // the key grid is the busiest screen: 48 keys plus the bottom bar
Target targets[kMaxTargets];
int count = 0;

// Gestures from the button task to the main loop: one writer, one reader.
constexpr int kQueue = 8;
Event queue[kQueue];
volatile uint8_t head = 0, tail = 0;

void push(Gesture g, int x = 0, int y = 0) {
  const uint8_t next = (head + 1) % kQueue;
  if (next == tail) return;  // full: the main loop is busy; drop it like a missed tap
  queue[head] = Event{g, (int16_t)x, (int16_t)y};
  head = next;
}

// The touch controller reports in the panel's native (landscape) frame; the screens are drawn in portrait the way gfx
// turns them: portrait x runs against the panel's y, portrait y along its x.
void toPortrait(float nx, float ny, int& x, int& y) {
  x = (int)((1.0f - ny) * gfx::W);
  y = (int)(nx * gfx::H);
}
}  // namespace

void clear() { count = 0; }

void target(int x, int y, int w, int h, int* sel, int value, uint8_t button) {
  if (count < kMaxTargets) targets[count++] = Target{(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, sel, nullptr, value, button};
}

void targetFn(int x, int y, int w, int h, void (*select)(int), int value, uint8_t button) {
  if (count < kMaxTargets) targets[count++] = Target{(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, nullptr, select, value, button};
}

bool tap(int x, int y, uint8_t& button) {
  for (int i = count - 1; i >= 0; i--) {
    const Target& t = targets[i];
    if (x >= t.x && x < t.x + t.w && y >= t.y && y < t.y + t.h) {
      if (t.sel) *t.sel = t.value;
      if (t.select) t.select(t.value);
      button = t.button;
      return true;
    }
  }
  return false;
}

bool capture(const InputManager& input) {
  bool any = false;
  float sx, sy, ex, ey;
  if (input.wasSwipe(sx, sy, ex, ey)) {
    int x0, y0, x1, y1;
    toPortrait(sx, sy, x0, y0);
    toPortrait(ex, ey, x1, y1);
    const int dx = x1 - x0, dy = y1 - y0;
    if (abs(dx) >= abs(dy)) push(dx < 0 ? Gesture::SwipeLeft : Gesture::SwipeRight);
    else push(dy < 0 ? Gesture::SwipeUp : Gesture::SwipeDown);
    any = true;
  } else if (input.wasTouchTap(sx, sy)) {
    int x, y;
    toPortrait(sx, sy, x, y);
    push(Gesture::Tap, x, y);
    any = true;
  }
  if (input.wasHomeKeyTapped()) { push(Gesture::Home); any = true; }
  if (input.wasHomeKeyLongPressed()) { push(Gesture::HomeLong); any = true; }
  return any;
}

void inject(const Event& e) { push(e.gesture, e.x, e.y); }

bool pop(Event& e) {
  if (tail == head) return false;
  e = queue[tail];
  tail = (tail + 1) % kQueue;
  return true;
}
}  // namespace touch
#endif
