#include "ui.h"

#include <math.h>

#include "gfx.h"

namespace ui {
namespace {
// Hints sit directly above the four buttons along the bottom edge: Back, OK, Left, Right.
// The four front buttons' positions under a 480 px wide screen, scaled to the panel's width.
constexpr int kButtonX480[4] = {25, 130, 245, 350}, kButtonW480 = 106;
int buttonX(int i) { return kButtonX480[i] * gfx::W / 480; }
int buttonW() { return kButtonW480 * gfx::W / 480; }

// Icons drawn with float math (slow in software: the C3 has no FPU) are worked out once into a 1-bit mask around their
// centre and stamped from then on. A mask is keyed by the icon and its scale.
struct IconMask {
  int key = -1, r = 0;  // the mask covers -r..r around the centre
  uint8_t* bits = nullptr;
};
IconMask iconMasks[6];

template <typename Draw>
void stampIcon(int key, int r, int cx, int cy, Draw draw) {
  IconMask* m = nullptr;
  for (auto& k : iconMasks) if (k.key == key) { m = &k; break; }
  const int side = 2 * r + 1, stride = (side + 7) / 8;
  if (!m) {
    for (auto& k : iconMasks) if (k.key < 0) { m = &k; break; }
    uint8_t* bits = m ? static_cast<uint8_t*>(calloc(side, stride)) : nullptr;
    if (!bits) {  // no room to keep it: draw it directly
      draw([&](int dx, int dy) { gfx::fillRect(cx + dx, cy + dy, 1, 1, true); });
      return;
    }
    draw([&](int dx, int dy) {
      if (dx >= -r && dx <= r && dy >= -r && dy <= r) bits[(dy + r) * stride + (dx + r) / 8] |= 0x80 >> ((dx + r) & 7);
    });
    m->key = key;
    m->r = r;
    m->bits = bits;
  }
  for (int y = 0; y < side; y++)
    for (int x = 0; x < side; x++)
      if (m->bits[y * stride + x / 8] & (0x80 >> (x & 7))) gfx::fillRect(cx - r + x, cy - r + y, 1, 1, true);
}
}  // namespace

void triangle(int cx, int cy, Dir d, bool black) {  // 13 px wide/high pointing arrow
  for (int i = 0; i < 13; i++) {
    const int half = i / 2;  // i = 0 is the tip, widening to the base
    switch (d) {
      case Dir::Up: gfx::fillRect(cx - half, cy - 6 + i, half * 2 + 1, 1, black); break;
      case Dir::Down: gfx::fillRect(cx - half, cy + 6 - i, half * 2 + 1, 1, black); break;
      case Dir::Left: gfx::fillRect(cx - 6 + i, cy - half, 1, half * 2 + 1, black); break;
      case Dir::Right: gfx::fillRect(cx + 6 - i, cy - half, 1, half * 2 + 1, black); break;
    }
  }
}

void drawSyncIcon(int cx, int cy, float f, bool slash) {
  stampIcon((int)(f * 100), (int)(16 * f) + 2, cx, cy, [f](auto plot) {
    constexpr float kPi = 3.14159265f;
    const float kR = 9.5f * f, thick = max(1.2f, 1.8f * f);
    const int reach = (int)(14 * f) + 2;
    for (int dy = -reach; dy <= reach; dy++)
      for (int dx = -reach; dx <= reach; dx++) {
        const float d = sqrtf((float)(dx * dx + dy * dy));
        if (d < kR - thick || d > kR + thick) continue;
        float a = atan2f((float)dy, (float)dx) * 180.0f / kPi;  // -180..180, clockwise on screen
        if (a < 0) a += 360.0f;
        if ((a > 25 && a < 150) || (a > 205 && a < 330)) plot(dx, dy);
      }
    for (int k = 0; k < 2; k++) {  // an arrow head at the end of each arc, pointing clockwise
      const float a = (k ? 330.0f : 150.0f) * kPi / 180.0f;
      const float px = kR * cosf(a), py = kR * sinf(a);
      const float tx = -sinf(a), ty = cosf(a), nx = cosf(a), ny = sinf(a);  // tangent (direction of travel) and radial
      const float tipX = px + 6 * f * tx, tipY = py + 6 * f * ty;
      const float b1x = px - 5 * f * nx, b1y = py - 5 * f * ny, b2x = px + 5 * f * nx, b2y = py + 5 * f * ny;
      const int r2 = (int)(16 * f) + 2;
      for (int y = -r2; y <= r2; y++)
        for (int x = -r2; x <= r2; x++) {  // inside the triangle (tip, b1, b2)?
          const float d1 = (x - b2x) * (tipY - b2y) - (tipX - b2x) * (y - b2y);
          const float d2 = (x - b1x) * (b2y - b1y) - (b2x - b1x) * (y - b1y);
          const float d3 = (x - tipX) * (b1y - tipY) - (b1x - tipX) * (y - tipY);
          if (!((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0))) plot(x, y);
        }
    }
  });
  if (slash) {  // crossed out: a diagonal with a white edge so it reads over the arrows
    const int n = (int)(12 * f);
    for (int i = -n; i <= n; i++) gfx::fillRect(cx + i - 2, cy + i - 3, 6, 7, false);  // the white edge first...
    for (int i = -n; i <= n; i++) gfx::fillRect(cx + i, cy + i - 1, 2, 3, true);       // ...then the line on top
  }
}

void drawHints(bool back, bool ok, bool paging, bool settings, bool sync) {
  const int top = gfx::H - 52, y = gfx::H - 42;
  gfx::fillRect(0, top, gfx::W, 2, true);
  const auto center = [](int i, int w) { return buttonX(i) + (buttonW() - w) / 2; };
  // Back: a return arrow (shaft out to the right, down, and back left) so it isn't mistaken for the Left arrow.
  const int bx = center(0, 30), by = y + 6;
  if (back) {
    triangle(bx + 6, by, Dir::Left, true);
    gfx::fillRect(bx + 8, by - 1, 20, 3, true);
    gfx::fillRect(bx + 26, by - 1, 3, 15, true);
    gfx::fillRect(bx + 10, by + 11, 19, 3, true);
  }
  // OK: a solid tick.
  const int ox = buttonX(1) + buttonW() / 2, oy = y + 14;
  if (ok) {
    for (int i = 0; i < 7; i++) gfx::fillRect(ox - 13 + i, oy - 1 + i, 4, 4, true);       // short stroke down-right
    for (int i = 0; i < 15; i++) gfx::fillRect(ox - 7 + i, oy + 5 - i * 17 / 15, 4, 4, true);  // long stroke up-right
  }
  if (paging) {
    triangle(buttonX(2) + buttonW() / 2, y + 14, Dir::Left, true);
    triangle(buttonX(3) + buttonW() / 2, y + 14, Dir::Right, true);
  }
  if (sync) drawSyncIcon(buttonX(2) + buttonW() / 2, y + 14, 1.0f, false);
  if (settings) {  // a gear above the fourth button
    const int gx = buttonX(3) + buttonW() / 2, gy = y + 14;
    stampIcon(1, 13, gx, gy, [](auto plot) {
      for (int dy = -13; dy <= 13; dy++)
        for (int dx = -13; dx <= 13; dx++) {
          const float d = sqrtf((float)(dx * dx + dy * dy));
          bool on = d <= 9.0f && d >= 4.5f;  // the ring with its hub hole
          if (!on && d > 9.0f && d <= 13.0f) {  // eight teeth
            float a = atan2f((float)dy, (float)dx) * 180.0f / (float)PI;  // -180..180
            a = fmodf(a + 360.0f + 22.5f, 45.0f);
            on = a > 11.0f && a < 34.0f;
          }
          if (on) plot(dx, dy);
        }
    });
  }
}

// A small "saved on the device" icon: an arrow dropping into a tray.
void drawDownloadedIcon(int x, int y, bool ink) {
  gfx::fillRect(x + 6, y, 4, 8, ink);                                          // arrow shaft
  for (int r = 0; r < 6; r++) gfx::fillRect(x + 8 - (6 - r), y + 7 + r, 2 * (6 - r), 1, ink);  // arrow head
  gfx::fillRect(x, y + 15, 16, 2, ink);                                         // tray bottom
  gfx::fillRect(x, y + 11, 2, 6, ink);                                          // tray sides
  gfx::fillRect(x + 14, y + 11, 2, 6, ink);
}

// Text in a normal row (black) or a highlighted one (white on the black highlight).
void label(int x, int y, const char* s, bool highlighted) { gfx::text(x, y, s, !highlighted); }

// Filled rectangle with rounded corners of radius r.
void roundRect(int x, int y, int w, int h, int r, bool black) {
  for (int j = 0; j < h; j++) {
    const int fromEdge = j < r ? r - j : (j >= h - r ? j - (h - r) + 1 : 0);
    int inset = 0;
    if (fromEdge > 0) inset = r - (int)sqrtf((float)(r * r - (fromEdge - 0.5f) * (fromEdge - 0.5f)));
    gfx::fillRect(x + inset, y + j, w - 2 * inset, 1, black);
  }
}

// A 2 px outline with slightly rounded corners; `solid` fills it black (the highlighted state).
void frame(int x, int y, int w, int h, int r, bool solid) {
  roundRect(x, y, w, h, r, true);
  if (!solid) roundRect(x + 2, y + 2, w - 4, h - 4, max(0, r - 2), false);
}

void card(int x, int y, int w, int h, const char* title) {
  frame(x, y, w, h);
  gfx::text(x + 10, y + 5, gfx::ellipsize(title, w - 20));
}

void bar(int x, int y, int w, int h, float fraction, bool inverse) {
  if (inverse) {  // white outline and fill on a dark background
    roundRect(x, y, w, h, 4, false);
    roundRect(x + 2, y + 2, w - 4, h - 4, 2, true);
  } else {
    frame(x, y, w, h, 4);
  }
  const int fill = (int)((w - 8) * constrain(fraction, 0.0f, 1.0f));
  if (fill > 0) roundRect(x + 4, y + 4, fill, h - 8, 2, !inverse);
}

void dot(int cx, int cy, int r, bool filled) {
  for (int dy = -r; dy <= r; dy++)
    for (int dx = -r; dx <= r; dx++) {
      const int d2 = dx * dx + dy * dy;
      if (filled ? d2 <= r * r : (d2 <= r * r && d2 > (r - 2) * (r - 2))) gfx::fillRect(cx + dx, cy + dy, 1, 1, true);
    }
}

// Shortens `s` in place with "..." until it fits `maxW` at the given large size (UTF-8 safe).
void fitBig(char* s, int maxW, int size) {
  if (gfx::bigTextWidth(s, size) <= maxW) return;
  size_t n = strlen(s);
  while (n > 0) {
    n--;
    while (n > 0 && (s[n] & 0xC0) == 0x80) n--;  // back to the start of a character
    char tmp[100];
    snprintf(tmp, sizeof(tmp), "%.*s...", (int)n, s);
    if (gfx::bigTextWidth(tmp, size) <= maxW || n == 0) {
      strlcpy(s, tmp, 100);
      return;
    }
  }
}

// Draws `text` word-wrapped into `w` pixels from (x, y); returns the y below the last line.
int wrapText(int x, int y, int w, const String& text) {
  String line;
  int start = 0;
  while (start <= (int)text.length()) {
    int sp = text.indexOf(' ', start);
    if (sp < 0) sp = text.length();
    const String word = text.substring(start, sp);
    const String tryLine = line.isEmpty() ? word : line + " " + word;
    if (gfx::textWidth(tryLine.c_str()) > w && !line.isEmpty()) {
      gfx::text(x, y, line.c_str());
      y += gfx::lineHeight();
      line = word;
    } else {
      line = tryLine;
    }
    start = sp + 1;
  }
  if (!line.isEmpty()) {
    gfx::text(x, y, line.c_str());
    y += gfx::lineHeight();
  }
  return y;
}

// Splits `text` over at most two lines of `maxW` at the given large size (the second shortened with "..."); returns the line count.
int wrapBig(const char* text, int maxW, int size, char* l1, char* l2) {
  strlcpy(l1, text, 100);
  l2[0] = 0;
  if (gfx::bigTextWidth(l1, size) > maxW) {
    int cut = -1;  // the last space after which the first line still fits
    for (int i = 0; l1[i]; i++) {
      if (l1[i] != ' ') continue;
      l1[i] = 0;
      const bool fits = gfx::bigTextWidth(l1, size) <= maxW;
      l1[i] = ' ';
      if (!fits) break;
      cut = i;
    }
    if (cut > 0) {
      strlcpy(l2, text + cut + 1, 100);
      l1[cut] = 0;
    }
  }
  fitBig(l1, maxW, size);
  fitBig(l2, maxW, size);
  return l2[0] ? 2 : 1;
}
}  // namespace ui
