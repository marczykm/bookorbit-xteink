#pragma once
// Reading companions from the BookOrbit KOReader plugin, as 64x64 1-bit sprites (see tools/make_pets.py).
#include <stdint.h>

namespace pets {
constexpr int kCount = 17, kPoses = 6, kBytes = 512;
enum Pose { Wait, Sleep, Ribbon, Party, Yawn, Read };
extern const char* const kNames[kCount];
extern const uint8_t kSprites[kCount][kPoses][kBytes];
}  // namespace pets
