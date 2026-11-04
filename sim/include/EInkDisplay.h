#pragma once
// Simulator: the panel. The framebuffer has the real panel's layout (landscape, 1 bit per pixel, 1 = white); a refresh hands
// it to the simulator window.
#include <stdint.h>

namespace freeink {
class FreeInkDisplay {
 public:
  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };

  FreeInkDisplay(int, int, int, int, int, int) {}
  void setDisplayX3();
  bool isX3Mode() const { return x3_; }
  void begin();
  uint8_t* getFrameBuffer() { return fb_; }
  uint16_t getDisplayWidth() const { return width_; }
  uint16_t getDisplayHeight() const { return height_; }
  uint16_t getDisplayWidthBytes() const { return (width_ + 7) / 8; }
  uint32_t getBufferSize() const { return (uint32_t)getDisplayWidthBytes() * height_; }
  void displayBuffer(RefreshMode mode = FAST_REFRESH);
  void displayBufferAsync(RefreshMode mode = FAST_REFRESH);
  bool refreshBusy() const;
  void waitRefreshComplete();
  bool supportsAsyncRefresh() const { return !x3_; }
  void setAsyncShadowAllowed(bool on) { shadowAllowed_ = on; }
  bool asyncShadowReady() const { return !x3_ && shadowAllowed_; }
  void deepSleep() {}

 private:
  bool x3_ = false, shadowAllowed_ = true;
  uint16_t width_ = 800, height_ = 480;
  uint8_t fb_[100 * 528];
  unsigned long busyUntil_ = 0;
};
}  // namespace freeink

using EInkDisplay = freeink::FreeInkDisplay;
