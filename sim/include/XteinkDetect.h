#pragma once
// Simulator: the device is chosen on the command line (--x3).
namespace freeink {
bool selectXteinkDevice();
inline bool applyXteinkDisplayController() { return false; }
}  // namespace freeink

namespace BoardConfig {
inline void releaseSdRail() {}
}  // namespace BoardConfig
