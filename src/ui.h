#pragma once
// Drawing building blocks shared by the screens: shapes, icons, the button hints and text layout helpers (on top of gfx).
#include <Arduino.h>

namespace ui {
enum class Dir { Up, Down, Left, Right };

void triangle(int cx, int cy, Dir d, bool black);  // a 13 px pointing arrow centred on (cx, cy)
void roundRect(int x, int y, int w, int h, int r, bool black);  // filled, with rounded corners of radius r
// A 2 px outline with slightly rounded corners; `solid` fills it black (the highlighted state).
void frame(int x, int y, int w, int h, int r = 8, bool solid = false);
void card(int x, int y, int w, int h, const char* title);  // a frame with a title in its top-left corner
void bar(int x, int y, int w, int h, float fraction, bool inverse = false);  // a progress bar; inverse = for a dark background
void dot(int cx, int cy, int r, bool filled);
// Two circular arrows (sync), scaled by f (1 = about 28 px across), centred on (cx, cy); `slash` crosses them out.
void drawSyncIcon(int cx, int cy, float f, bool slash);
// Icons along the bottom edge, above the four buttons: Back, OK and Left/Right (paging). `menu` shows a menu icon over the
// first button instead of Back.
void drawHints(bool back = true, bool ok = true, bool paging = true, bool menu = false);
void drawDownloadedIcon(int x, int y, bool ink);  // "saved on the device": an arrow dropping into a tray
void label(int x, int y, const char* s, bool highlighted);  // text in a normal row, or white on a highlighted one
// Draws `text` word-wrapped into `w` pixels from (x, y); returns the y below the last line.
int wrapText(int x, int y, int w, const String& text);
// Shortens `s` (a 100-byte buffer) in place with "..." until it fits `maxW` at the given large size (UTF-8 safe).
void fitBig(char* s, int maxW, int size);
// Splits `text` over at most two lines of `maxW` at the given large size (the second shortened with "..."); returns the line
// count. l1 and l2 are 100-byte buffers.
int wrapBig(const char* text, int maxW, int size, char* l1, char* l2);
}  // namespace ui
