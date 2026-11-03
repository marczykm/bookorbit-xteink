#pragma once
// Portrait (480x800) drawing on the X4's native 800x480 1-bit framebuffer.
// Text uses FreeInkUI's built-in Noto Sans bitmap (ASCII only, Polish letters folded) until loadUiFont() switches to Noto Serif.
#include <EInkDisplay.h>
#include <stdint.h>

namespace gfx {
constexpr int W = 480;
constexpr int H = 800;

void begin(EInkDisplay* display);
// Switches text to Noto Serif from the flash font pack (Polish letters); keeps the ASCII bitmap font on failure.
bool loadUiFont();
void clear(bool black = false);  // white page, or a black one
// Draws a 1-bit image (MSB first, 1 = black pixel) with its top-left at (x, y).
void blit1bit(int x, int y, const uint8_t* bits, int w, int h);
void fillRect(int x, int y, int w, int h, bool black);
void text(int x, int y, const char* utf8, bool black = true);  // y = top of the line
int textWidth(const char* utf8);
// Wi-Fi fan icon, 26x20 px with its top-left at (x, y): `arcs` 0-3 signal arcs (0 = just the dot), `slash` crosses it out (solid); `grey` dithers the arcs and dot.
void wifiIcon(int x, int y, int arcs, bool slash, bool black, bool grey = false);
// Battery icon, 38x18 px with its top-left at (x, y), filled to `percent` (0-100).
void batteryIcon(int x, int y, int percent, bool black);
int lineHeight();
// Large text horizontally centred with its top at y (needs loadUiFont(); falls back to a scaled bitmap font).
void bigTextCentered(int y, const char* utf8, int sizePx, bool black = true);
// Same at an explicit x (left edge), and its width.
void bigText(int x, int y, const char* utf8, int sizePx, bool black = true);
int bigTextWidth(const char* utf8, int sizePx);
// Fits `utf8` into maxWidth pixels, ending with "..." when it has to cut. Returns a static buffer.
const char* ellipsize(const char* utf8, int maxWidth);
// Starts a panel refresh and returns at once (the panel finishes on its own, ~0.5-2 s), so buttons keep being read meanwhile.
void flush(EInkDisplay::RefreshMode mode);
bool busy();      // a refresh is still running on the panel
// Low-memory mode (during network transfers): frees the 48 KB refresh shadow and refreshes block until it is switched off.
void setLowMemory(bool on);
void waitIdle();  // block until it is done
void dumpFramebuffer();  // hex over serial, for screenshots without a camera
}  // namespace gfx
