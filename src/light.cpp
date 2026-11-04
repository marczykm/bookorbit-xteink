#include "light.h"

#include <Arduino.h>
#include <SDCardManager.h>

#if __has_include(<FrontlightManager.h>)
#include <FrontlightManager.h>
#define BOOKORBIT_LIGHT 1
#endif

namespace light {
namespace {
constexpr const char* kFile = "/BookOrbit/.light";  // "<brightness> <warmth>"
int level = 0, warm = 50;
#if BOOKORBIT_LIGHT
FrontlightManager fl;
#endif

void apply() {
#if BOOKORBIT_LIGHT
  if (fl.hasColorTemperature()) fl.setColorTemperature(warm);
  fl.setBrightness(level);
#endif
}
}  // namespace

bool available() {
#if BOOKORBIT_LIGHT
  return fl.present();
#else
  return false;
#endif
}

bool hasWarmth() {
#if BOOKORBIT_LIGHT
  return fl.hasColorTemperature();
#else
  return false;
#endif
}

void begin() {
  if (!available()) return;
#if BOOKORBIT_LIGHT
  fl.begin();
#endif
  if (SdMan.exists(kFile)) {
    const String s = SdMan.readFile(kFile);
    level = constrain((int)s.toInt(), 0, 100);
    const int sp = s.indexOf(' ');
    if (sp > 0) warm = constrain((int)s.substring(sp + 1).toInt(), 0, 100);
  }
  apply();
}

int brightness() { return level; }
int warmth() { return warm; }

void set(int b, int w) {
  level = constrain(b, 0, 100);
  warm = constrain(w, 0, 100);
  apply();
}

void save() {
  if (!available()) return;
  SdMan.ensureDirectoryExists("/BookOrbit");
  SdMan.writeFile(kFile, String(level) + " " + String(warm));
}

void sleep() {
#if BOOKORBIT_LIGHT
  if (available()) fl.prepareForDeepSleep();
#endif
}
}  // namespace light
