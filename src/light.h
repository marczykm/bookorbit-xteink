#pragma once
// The frontlight (X4 Pro: warm and cool LEDs): brightness and warmth in steps of 10%, kept on the SD card. On boards
// without one this is all a no-op and available() is false.

namespace light {
bool available();
bool hasWarmth();  // a warm/cool pair, not just one colour
void begin();      // at boot (SD card mounted): restores the saved brightness and warmth
int brightness();  // 0 (off) - 100
int warmth();      // 0 (cool) - 100 (warm)
void set(int brightness, int warmth);  // applies at once
void save();       // writes the current values to the SD card
void sleep();      // turns the light off for deep sleep (the next boot restores it)
}  // namespace light
